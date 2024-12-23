# SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.

# SPDX-License-Identifier: Apache-2.0

import torch
import pytest
from loguru import logger
import ttnn
from tests.tt_eager.python_api_testing.sweep_tests.comparison_funcs import comp_equal, comp_pcc
from models.utility_functions import skip_for_grayskull
from tests.ttnn.unit_tests.operations.ccl.test_reduce_scatter_async import (
    create_and_load_sub_device_manager_with_fabric_interface,
    teardown_fabric_interface,
)


def is_unsupported_case(input_shape, dim, mem_config, num_devices, num_links, input_dtype, layout):
    if layout == ttnn.ROW_MAJOR_LAYOUT and input_dtype == ttnn.bfloat8_b:
        return True, "Invalid combination"

    if input_shape[dim] % num_devices != 0 or (dim == 3 and input_shape[dim] // num_devices % 32 != 0):
        return True, "Unsupported test case"

    ## Check that we can readback results
    fast_dispatch_page_size_limit = 55 * 1024
    elem_size = 2 if input_dtype == ttnn.bfloat16 else 1
    if layout == ttnn.ROW_MAJOR_LAYOUT and (input_shape[dim] * elem_size) > fast_dispatch_page_size_limit:
        # Fast dispatch currently can't breakup readback of large pages into multiple smaller pages and is
        # limited to ~55K pages.
        return True, "Fast dispatch can't support reading back this page size in one shot"

    # Check that we can fit in L1 (if L1 config)
    tensor_size_bytes = elem_size
    for i in input_shape:
        tensor_size_bytes *= i
    num_l1_banks = 64
    if mem_config.buffer_type == ttnn.BufferType.L1 and tensor_size_bytes > num_l1_banks * 50 * 1024:
        return True, "L1 buffer can't support large tensor sizes"

    # Check that each chip has a non-zero amount of data available
    min_sized_chunks_on_dim = input_shape[dim]
    if dim == 3:
        min_sized_chunks_on_dim //= 32
    if dim == 2:
        if layout == ttnn.TILE_LAYOUT:
            min_sized_chunks_on_dim //= 32
    if min_sized_chunks_on_dim < num_devices:
        return (
            True,
            f"Input shape {input_shape} incompatible with {num_devices} on dim {dim} because some chips will have no tensor",
        )

    if input_shape == [8, 8, 256, 384] and dim == 1 and layout == ttnn.TILE_LAYOUT and input_dtype == ttnn.bfloat8_b:
        return True, "Known failure"

    return False, ""


def run_with_trace(
    mesh_device,
    all_gather_topology,
    input_tensor_mesh,
    dim,
    num_links,
    output_mem_config,
    num_iter=20,
    subdevice_id=None,
):
    # Compile Run
    logger.info("Compiling model")
    tt_out_tensor = ttnn.experimental.all_gather_async(
        input_tensor_mesh,
        dim,
        num_links=num_links,
        memory_config=output_mem_config,
        topology=all_gather_topology,
        subdevice_id=subdevice_id,
        create_semaphore_handles=True,
    )
    for d in mesh_device.get_devices():
        ttnn.synchronize_device(d)

    # Capture trace
    logger.info("Capturing trace")
    trace_id = ttnn.begin_trace_capture(mesh_device, cq_id=0)
    for i in range(num_iter):
        tt_out_tensor = ttnn.experimental.all_gather_async(
            input_tensor_mesh,
            dim,
            num_links=num_links,
            memory_config=output_mem_config,
            topology=all_gather_topology,
            subdevice_id=subdevice_id,
            create_semaphore_handles=False,
        )
    ttnn.end_trace_capture(mesh_device, trace_id, cq_id=0)
    for d in mesh_device.get_devices():
        ttnn.synchronize_device(d)

    # Run the op
    logger.info("Starting Trace perf test...")
    ttnn.execute_trace(mesh_device, trace_id, blocking=False)
    ttnn.release_trace(mesh_device, trace_id)
    for d in mesh_device.get_devices():
        ttnn.synchronize_device(d)

    return tt_out_tensor


def run_all_gather_impl(
    mesh_device,
    num_devices,
    output_shape,
    dim,
    num_links,
    input_dtype,
    layout,
    use_program_cache,
    function_level_defaults,
    all_gather_topology,
    num_iters=1,
    enable_async=False,
    trace_mode=False,
    rand_tensor=True,
    mem_config=None,
    input_shard_shape=None,
    shard_grid=None,
    tensor_mem_layout=None,
    use_cluster_axis_api=False,
    cluster_axis=None,
    create_persistent_fabric=True,
    teardown_persistent_fabric=True,
):
    enable_persistent_fabric = True
    if num_iters < 1:
        pytest.fail("num_iters must be >= 1")
    # Use Async mode based on test input config
    mesh_device.enable_async(enable_async)

    if enable_async:
        logger.info(f"Using Async Mode for All Gather Op Dispatch")

    logger.info(f"Output shape: {output_shape}")
    logger.info(f"dim: {dim}")
    logger.info(f"input_shard_shape: {input_shard_shape}")
    logger.info(f"shard_grid: {shard_grid}")

    ### For sharded all gather only
    if bool(input_shard_shape) != bool(shard_grid) and bool(tensor_mem_layout) != bool(shard_grid):
        pytest.fail(
            "Both input_shard_shape, shard_grid, and tensor_mem_layout must be provided together or all must be None"
        )
    if input_shard_shape and shard_grid:
        input_shard_spec = ttnn.ShardSpec(
            shard_grid,
            input_shard_shape,
            ttnn.ShardOrientation.ROW_MAJOR,
            False,
        )
        input_mem_config = ttnn.MemoryConfig(
            tensor_mem_layout, buffer_type=ttnn.BufferType.L1, shard_spec=input_shard_spec
        )
        output_shard_shape = list(input_shard_shape)
        if dim == 3:
            output_shard_shape[1] *= num_devices
        else:
            output_shard_shape[0] *= num_devices
        output_shard_spec = ttnn.ShardSpec(
            shard_grid,
            output_shard_shape,
            ttnn.ShardOrientation.ROW_MAJOR,
            False,
        )
        output_mem_config = ttnn.MemoryConfig(
            tensor_mem_layout, buffer_type=ttnn.BufferType.L1, shard_spec=output_shard_spec
        )
    else:
        assert mem_config is not None
        input_mem_config = mem_config
        output_mem_config = mem_config
    ###

    if rand_tensor:
        output_tensor = torch.rand(output_shape).bfloat16()
    else:
        output_tensor = torch.zeros(output_shape)
        tile_id = 1
        for w in range(output_shape[0]):
            for z in range(output_shape[1]):
                for y in range(0, output_shape[2], 32):
                    for x in range(0, output_shape[3], 32):
                        output_tensor[w, z, y : y + 32, x : x + 32] = tile_id
                        tile_id += 1

    input_tensors = torch.chunk(output_tensor, num_devices, dim)
    tt_input_tensors = []
    for i, t in enumerate(input_tensors):
        tt_input_tensors.append(
            ttnn.Tensor(t, input_dtype).to(layout).to(mesh_device.get_devices()[i], input_mem_config)
        )
        logger.info(f"using device {mesh_device.get_devices()[i].id()}")

    input_tensor_mesh = ttnn.aggregate_as_tensor(tt_input_tensors)

    compute_grid_size = mesh_device.compute_with_storage_grid_size()
    worker_sub_device = ttnn.SubDevice(
        [
            ttnn.CoreRangeSet(
                {ttnn.CoreRange(ttnn.CoreCoord(0, 0), ttnn.CoreCoord(compute_grid_size.x - 1, compute_grid_size.y - 1))}
            )
        ]
    )
    worker_sub_device_id = ttnn.SubDeviceId(0)
    if create_persistent_fabric:
        mesh_sub_device_manager_id = create_and_load_sub_device_manager_with_fabric_interface(
            mesh_device, [worker_sub_device], 0, 0, enable_persistent_fabric
        )

    if trace_mode:
        tt_out_tensor = run_with_trace(
            mesh_device,
            all_gather_topology,
            input_tensor_mesh,
            dim,
            num_links,
            output_mem_config,
            num_iter=num_iters,
            subdevice_id=worker_sub_device_id,
        )
    else:
        for i in range(num_iters):
            if use_cluster_axis_api:
                tt_out_tensor = ttnn.experimental.all_gather_async(
                    input_tensor_mesh,
                    dim,
                    cluster_axis=cluster_axis,
                    mesh_device=mesh_device,
                    memory_config=output_mem_config,
                    topology=all_gather_topology,
                    subdevice_id=worker_sub_device_id,
                    enable_persistent_fabric_mode=enable_persistent_fabric,
                    num_preferred_links=num_links,
                    create_semaphore_handles=True,
                )

            else:
                tt_out_tensor = ttnn.experimental.all_gather_async(
                    input_tensor_mesh,
                    dim,
                    num_links=num_links,
                    memory_config=output_mem_config,
                    topology=all_gather_topology,
                    subdevice_id=worker_sub_device_id,
                    enable_persistent_fabric_mode=enable_persistent_fabric,
                )

            logger.info(f"Waiting for op {i}")
            for d in mesh_device.get_devices():
                ttnn.synchronize_device(d, sub_device_ids=[worker_sub_device_id])
            logger.info(f"Done iteration {i}")

    if enable_persistent_fabric and teardown_persistent_fabric:
        teardown_fabric_interface(mesh_device)

    for i, t in enumerate(ttnn.get_device_tensors(tt_out_tensor)):
        tt_output_tensor = t.cpu().to(ttnn.ROW_MAJOR_LAYOUT).to_torch()
        logger.info(f"Checking for device {t.device().id()}")

        if input_dtype == ttnn.bfloat16:
            eq, output = comp_equal(tt_output_tensor, output_tensor)
        else:
            eq, output = comp_pcc(tt_output_tensor, output_tensor)
        if not eq:
            logger.error(f"output mismatch for tensor {i}")
        assert eq, f"{i} FAILED: {output}"


# Enumerate the post-commit cases explicitly
@skip_for_grayskull("Requires eth connected devices to run")
@pytest.mark.parametrize(
    "num_devices, num_links, output_shape, dim, layout",
    [
        (4, 1, [1, 1, 64, 512], 3, ttnn.TILE_LAYOUT),
        # (4, 1, [1, 1, 32, 32768], 3, ttnn.TILE_LAYOUT),
        # (4, 1, [1, 1, 2048, 16384], 3, ttnn.TILE_LAYOUT),
    ],
)
@pytest.mark.parametrize(
    "input_dtype",
    [
        ttnn.bfloat16,
    ],
)
@pytest.mark.parametrize(
    "mem_config",
    [
        ttnn.MemoryConfig(buffer_type=ttnn.BufferType.DRAM),
    ],
)
@pytest.mark.parametrize("num_iters", [1])
@pytest.mark.parametrize("enable_async", [False])
def test_all_gather(
    t3k_mesh_device,
    # pcie_mesh_device,
    num_devices,
    output_shape,
    dim,
    num_links,
    input_dtype,
    layout,
    mem_config,
    num_iters,
    use_program_cache,
    function_level_defaults,
    enable_async,
):
    run_all_gather_impl(
        t3k_mesh_device,
        num_devices,
        output_shape,
        dim,
        num_links,
        input_dtype,
        layout,
        use_program_cache,
        function_level_defaults,
        all_gather_topology=ttnn.Topology.Ring,
        num_iters=num_iters,
        enable_async=enable_async,
        rand_tensor=True,
        mem_config=mem_config,
    )


# Enumerate the post-commit cases explicitly
@skip_for_grayskull("Requires eth connected devices to run")
@pytest.mark.parametrize(
    "num_devices, output_shape, dim, layout, input_shard_shape, shard_grid, tensor_mem_layout",
    [
        (
            2,
            [1, 1, 32, 256],
            3,
            ttnn.TILE_LAYOUT,
            (32, 32),
            ttnn.CoreRangeSet({ttnn.CoreRange(ttnn.CoreCoord(0, 0), ttnn.CoreCoord(0, 3))}),
            ttnn.TensorMemoryLayout.WIDTH_SHARDED,
        ),
        (
            2,
            [1, 1, 32, 256],
            3,
            ttnn.TILE_LAYOUT,
            (32, 64),
            ttnn.CoreRangeSet({ttnn.CoreRange(ttnn.CoreCoord(0, 0), ttnn.CoreCoord(0, 1))}),
            ttnn.TensorMemoryLayout.WIDTH_SHARDED,
        ),
        (
            2,
            [1, 1, 32, 256],
            3,
            ttnn.TILE_LAYOUT,
            (32, 128),
            ttnn.CoreRangeSet({ttnn.CoreRange(ttnn.CoreCoord(0, 0), ttnn.CoreCoord(0, 0))}),
            ttnn.TensorMemoryLayout.WIDTH_SHARDED,
        ),
        (
            2,
            [1, 1, 64, 256],
            2,
            ttnn.TILE_LAYOUT,
            (32, 128),
            ttnn.CoreRangeSet({ttnn.CoreRange(ttnn.CoreCoord(0, 0), ttnn.CoreCoord(0, 1))}),
            ttnn.TensorMemoryLayout.WIDTH_SHARDED,
        ),
        (
            2,
            [1, 4, 32, 256],
            3,
            ttnn.TILE_LAYOUT,
            (32, 128),
            ttnn.CoreRangeSet({ttnn.CoreRange(ttnn.CoreCoord(0, 0), ttnn.CoreCoord(0, 3))}),
            ttnn.TensorMemoryLayout.HEIGHT_SHARDED,
        ),
    ],
)
@pytest.mark.parametrize("num_links", [1])
@pytest.mark.parametrize(
    "input_dtype",
    [
        ttnn.bfloat16,
    ],
)
@pytest.mark.parametrize("num_iters", [1])
@pytest.mark.parametrize("enable_async", [False])
def test_all_gather_sharded(
    t3k_mesh_device,
    # pcie_mesh_device,
    num_devices,
    output_shape,
    dim,
    num_links,
    input_dtype,
    layout,
    num_iters,
    use_program_cache,
    function_level_defaults,
    enable_async,
    input_shard_shape,
    shard_grid,
    tensor_mem_layout,
):
    run_all_gather_impl(
        t3k_mesh_device,
        num_devices,
        output_shape,
        dim,
        num_links,
        input_dtype,
        layout,
        use_program_cache,
        function_level_defaults,
        all_gather_topology=ttnn.Topology.Ring,
        num_iters=num_iters,
        enable_async=enable_async,
        rand_tensor=True,
        input_shard_shape=input_shard_shape,
        shard_grid=shard_grid,
        tensor_mem_layout=tensor_mem_layout,
    )


# # Enumerate the post-commit cases explicitly
# @skip_for_grayskull("Requires eth connected devices to run")
# @pytest.mark.parametrize(
#     "row_num_devices, row_num_links, row_output_shape, row_gather_dim, row_tensor_mem_layout",
#     [
#         (4, 1, [1, 1, 64, 1024], 3, ttnn.TILE_LAYOUT),
#         # (4, 1, [1, 1, 32, 32768], 3, ttnn.TILE_LAYOUT),
#         # (4, 1, [1, 1, 2048, 16384], 3, ttnn.TILE_LAYOUT),
#     ],
# )
# @pytest.mark.parametrize(
#     "col_num_devices, col_num_links, col_output_shape, col_gather_dim, col_tensor_mem_layout",
#     [
#         (8, 1, [1, 1, 64, 1024], 3, ttnn.TILE_LAYOUT),
#     ],
# )
# @pytest.mark.parametrize(
#     "input_dtype",
#     [
#         ttnn.bfloat16,
#     ],
# )
# @pytest.mark.parametrize(
#     "mem_config",
#     [
#         ttnn.MemoryConfig(buffer_type=ttnn.BufferType.DRAM),
#     ],
# )
# @pytest.mark.parametrize("num_iters", [1])
# @pytest.mark.parametrize("enable_async", [False])
# def test_back_to_back_row_and_col_all_gathers_on_galaxy_mesh_fabric(
#     tg_mesh_device,
#     num_devices,
#     row_num_devices, row_num_links, row_output_shape, row_gather_dim, row_tensor_mem_layout,
#     col_num_devices, col_num_links, col_output_shape, col_gather_dim, col_tensor_mem_layout,
#     mem_config,
#     input_dtype,
#     num_iters,
#     use_program_cache,
#     function_level_defaults,
#     enable_async,
#     tensor_mem_layout,
# ):

#     run_all_gather_impl(
#         tg_mesh_device,
#         row_num_devices,
#         row_output_shape,
#         row_gather_dim,
#         row_num_links,
#         input_dtype,
#         use_program_cache,
#         function_level_defaults,
#         mem_config=mem_config,
#         all_gather_topology=ttnn.Topology.Linear,
#         num_iters=num_iters,
#         enable_async=enable_async,
#         rand_tensor=True,
#         tensor_mem_layout=row_tensor_mem_layout,
#         use_cluster_axis_api=True,
#         cluster_axis=1,
#         create_persistent_fabric=True,
#         teardown_persistent_fabric=False
#     )

#     run_all_gather_impl(
#         tg_mesh_device,
#         col_num_devices,
#         col_output_shape,
#         col_gather_dim,
#         col_num_links,
#         input_dtype,
#         use_program_cache,
#         function_level_defaults,
#         mem_config=mem_config,
#         all_gather_topology=ttnn.Topology.Linear,
#         num_iters=num_iters,
#         enable_async=enable_async,
#         rand_tensor=True,
#         tensor_mem_layout=col_tensor_mem_layout,
#         use_cluster_axis_api=True,
#         cluster_axis=0,
#         create_persistent_fabric=False,
#         teardown_persistent_fabric=True
#     )


# # Enumerate the post-commit cases explicitly
# @skip_for_grayskull("Requires eth connected devices to run")
# @pytest.mark.parametrize(
#     "row_num_devices, row_num_links, row_output_shape, row_gather_dim, row_layout",
#     [
#         (4, 1, [1, 1, 64, 1024], 3, ttnn.TILE_LAYOUT),
#     ],
# )
# @pytest.mark.parametrize(
#     "col_num_devices, col_num_links, col_output_shape, col_gather_dim, col_layout",
#     [
#         (2, 1, [1, 1, 64, 1024], 3, ttnn.TILE_LAYOUT),
#     ],
# )
# @pytest.mark.parametrize(
#     "input_dtype",
#     [
#         ttnn.bfloat16,
#     ],
# )
# @pytest.mark.parametrize(
#     "mem_config",
#     [
#         ttnn.MemoryConfig(buffer_type=ttnn.BufferType.DRAM),
#     ],
# )

# @pytest.mark.parametrize("num_iters", [1])
# @pytest.mark.parametrize("enable_async", [False])
# def test_back_to_back_row_and_col_all_gathers_on_t3k_mesh_fabric(
#     t3k_mesh_device,
#     row_num_devices, row_num_links, row_output_shape, row_gather_dim, row_layout,
#     col_num_devices, col_num_links, col_output_shape, col_gather_dim, col_layout,
#     mem_config,
#     input_dtype,
#     num_iters,
#     use_program_cache,
#     function_level_defaults,
#     enable_async,
# ):

#     run_all_gather_impl(
#         t3k_mesh_device,
#         row_num_devices,
#         row_output_shape,
#         row_gather_dim,
#         row_num_links,
#         input_dtype,
#         row_layout,
#         use_program_cache,
#         function_level_defaults,
#         mem_config=mem_config,
#         all_gather_topology=ttnn.Topology.Linear,
#         num_iters=num_iters,
#         enable_async=enable_async,
#         rand_tensor=True,
#         use_cluster_axis_api=True,
#         cluster_axis=1,
#         create_persistent_fabric=True,
#         teardown_persistent_fabric=False
#     )

#     run_all_gather_impl(
#         t3k_mesh_device,
#         col_num_devices,
#         col_output_shape,
#         col_gather_dim,
#         col_num_links,
#         input_dtype,
#         col_layout,
#         use_program_cache,
#         function_level_defaults,
#         mem_config=mem_config,
#         all_gather_topology=ttnn.Topology.Linear,
#         num_iters=num_iters,
#         enable_async=enable_async,
#         rand_tensor=True,
#         use_cluster_axis_api=True,
#         cluster_axis=0,
#         create_persistent_fabric=False,
#         teardown_persistent_fabric=True
#     )