# SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.

# SPDX-License-Identifier: Apache-2.0

import torch
import pytest
from loguru import logger
import ttnn
from tests.tt_eager.python_api_testing.sweep_tests.comparison_funcs import comp_pcc
from models.utility_functions import skip_for_grayskull


def create_and_load_sub_device_manager_with_fabric_interface(
    mesh_device, worker_sub_devices, ccl_worker_sub_device_id, local_allocator_size, enable_persistent_fabric=True
):
    assert ccl_worker_sub_device_id < len(worker_sub_devices)
    mesh_sub_device_manager_id, fabric_subdevice_id = mesh_device.create_sub_device_manager_with_fabric(
        worker_sub_devices, local_allocator_size
    )
    # fabric sub-device id can also be queried from device, no need to explicitly pass it in
    mesh_device.load_sub_device_manager(mesh_sub_device_manager_id)
    if enable_persistent_fabric:
        ttnn.initialize_edm_fabric(mesh_device)
    return mesh_sub_device_manager_id


def teardown_fabric_interface(mesh_device):
    ttnn.teardown_edm_fabric(mesh_device)
    for device_id in mesh_device.get_device_ids():
        ttnn.synchronize_device(mesh_device.get_device(device_id))


def is_unsupported_case(input_shape, dim, math_op, mem_config, num_devices, num_links, input_dtype, layout):
    elem_size = 2 if input_dtype == ttnn.bfloat16 else 1
    tensor_size_bytes = elem_size
    for i in input_shape:
        tensor_size_bytes *= i
    num_l1_banks = 64
    if mem_config.buffer_type == ttnn.BufferType.L1 and tensor_size_bytes > num_l1_banks * 50 * 1024:
        return True, "L1 buffer can't support large tensor sizes"

    # if input_dtype == ttnn.bfloat8_b and tuple(input_shape) == (1, 1, 2048, 1024) and dim == 3:
    #     return True, "Known failure with bfp8_b data format"

    return False, ""


def run_with_trace(
    t3k_mesh_device,
    input_tensor_mesh,
    dim,
    num_links,
    math_op,
    output_mem_config,
    num_iters=40,
    topology=ttnn.Topology.Ring,
    subdevice_id=None,
):
    # Compile Run
    logger.info("Compiling model")
    output_tensor_mesh = ttnn.reduce_scatter_async(
        input_tensor_mesh,
        dim=dim,
        math_op=math_op,
        num_links=num_links,
        memory_config=output_mem_config,
        topology=topology,
        subdevice_id=subdevice_id,
        create_semaphore_handles=True,
    )
    for device_id in t3k_mesh_device.get_device_ids():
        ttnn.synchronize_device(t3k_mesh_device.get_device(device_id))

    # Capture trace
    logger.info("Capturing trace")
    trace_id = ttnn.begin_trace_capture(t3k_mesh_device, cq_id=0)
    for i in range(num_iters):
        output_tensor_mesh = ttnn.reduce_scatter_async(
            input_tensor_mesh,
            dim=dim,
            math_op=math_op,
            num_links=num_links,
            memory_config=output_mem_config,
            topology=topology,
            subdevice_id=subdevice_id,
            create_semaphore_handles=False,
        )
    ttnn.end_trace_capture(t3k_mesh_device, trace_id, cq_id=0)
    for device_id in t3k_mesh_device.get_device_ids():
        ttnn.synchronize_device(t3k_mesh_device.get_device(device_id))

    # Run the op
    logger.info("Starting Trace perf test...")
    ttnn.execute_trace(t3k_mesh_device, trace_id, blocking=False)
    ttnn.release_trace(t3k_mesh_device, trace_id)
    for device_id in t3k_mesh_device.get_device_ids():
        ttnn.synchronize_device(t3k_mesh_device.get_device(device_id))

    return output_tensor_mesh


def run_reduce_scatter_test(
    mesh_device,
    num_devices,
    per_chip_output_shape,
    dim,
    num_links,
    math_op,
    input_dtype,
    layout,
    mem_config,
    use_program_cache,
    function_level_defaults,
    enable_async=True,
    num_iters=1,
    topology=ttnn.Topology.Ring,
    trace_mode=False,
):
    enable_persistent_fabric = True
    if len(mesh_device.get_device_ids()) < num_devices:
        pytest.skip(
            f"Not enough devices on machine to implement test case. Wanted {num_devices} but found {len(mesh_device.get_device_ids())}"
        )

    debug = False

    (is_known_failure, message) = is_unsupported_case(
        per_chip_output_shape, dim, math_op, mem_config, num_devices, num_links, input_dtype, layout
    )
    if is_known_failure:
        pytest.skip(f"Skipping unsupported case {message}.")

    mesh_device.enable_async(enable_async)
    if enable_async:
        logger.info(f"Using Async Mode for Reduce Scatter Op Dispatch")

    logger.info(f"Per chip output shape: {per_chip_output_shape}, devices: {num_devices}, dim: {dim}")

    # Generate input tensors
    canonical_input_shape = per_chip_output_shape.copy()
    canonical_input_shape[dim] *= num_devices
    tt_input_tensors = []

    numel = canonical_input_shape[0] * canonical_input_shape[1] * canonical_input_shape[2] * canonical_input_shape[3]
    input_tensors = [
        torch.rand(canonical_input_shape).bfloat16() if not debug else torch.ones(canonical_input_shape).bfloat16()
        for _ in range(num_devices)
    ]
    if debug:
        tile_id = 0
        for w in range(input_tensors[-1].shape[0]):
            for z in range(input_tensors[-1].shape[1]):
                for y in range(0, input_tensors[-1].shape[2], 32):
                    for x in range(0, input_tensors[-1].shape[3], 32):
                        for yy in range(32):
                            for xx in range(32):
                                input_tensors[-1][w, z, y + yy, x + xx] = tile_id
                        # input_tensors[-1][w,z,y:y+32,x:x+32] = tile_id
                        tile_id += 1
    for i, canonical_input_tensor in enumerate(input_tensors):
        logger.info(f"Creating input tensor on device {mesh_device.get_device_ids()[i]}")
        tt_input_tensors.append(
            ttnn.Tensor(canonical_input_tensor, input_dtype)
            .to(layout)
            .to(mesh_device.get_device(mesh_device.get_device_ids()[i]), mem_config)
        )

    assert len(tt_input_tensors) == num_devices

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
    mesh_sub_device_manager_id = create_and_load_sub_device_manager_with_fabric_interface(
        mesh_device, [worker_sub_device], 0, 0, enable_persistent_fabric
    )

    # Run the op
    if trace_mode:
        output_tensor_mesh = run_with_trace(
            mesh_device,
            input_tensor_mesh,
            dim,
            num_links,
            math_op,
            mem_config,
            num_iters=num_iters,
            topology=topology,
            subdevice_id=ttnn.SubDeviceId(0),
        )
    else:
        for i in range(num_iters):
            output_tensor_mesh = ttnn.reduce_scatter_async(
                input_tensor_mesh,
                dim=dim,
                math_op=math_op,
                num_links=num_links,
                memory_config=mem_config,
                topology=topology,
                subdevice_id=worker_sub_device_id,
            )

            logger.info(f"Waiting for op {i}")
            for device_id in mesh_device.get_device_ids():
                ttnn.synchronize_device(mesh_device.get_device(device_id), sub_device_ids=[worker_sub_device_id])
            logger.info(f"Done iteration {i}")

    teardown_fabric_interface(mesh_device)
    # Compute golden
    # TODO: Make it model how reduce scatter actually works for numerical correctness/ordering
    golden_canonical_out_tensor = torch.zeros(canonical_input_shape).bfloat16()
    for i, t in enumerate(input_tensors):
        golden_canonical_out_tensor = torch.add(golden_canonical_out_tensor, t).bfloat16()

    golden_output_tensors = torch.chunk(golden_canonical_out_tensor, num_devices, dim)

    tt_out_tensors = ttnn.get_device_tensors(output_tensor_mesh)
    logger.info(f"Compare")
    # Compare
    assert len(golden_output_tensors) == len(tt_out_tensors)
    mismatch = False
    for i, t in enumerate(tt_out_tensors):
        logger.info(f"DEVICE {i}")
        logger.info(f"Checking output from device {t.device().id()}")
        tt_output_tensor = t.cpu().to(ttnn.ROW_MAJOR_LAYOUT).to_torch()
        eq, output = comp_pcc(tt_output_tensor, golden_output_tensors[i])
        mismatch = mismatch or not eq
        if not eq:
            logger.error(f"output mismatch for tensor {i}. Mesh device ID: {mesh_device.get_devices()[i].id()}")
            if debug:
                logger.info(f"FINAL OUTPUT TENSOR {tt_output_tensor}")
                mismatch_tensor_shape = [
                    tt_output_tensor.shape[0],
                    tt_output_tensor.shape[1],
                    tt_output_tensor.shape[2] // 32,
                    tt_output_tensor.shape[3] // 32,
                ]
                mismatch_tensor = torch.zeros(mismatch_tensor_shape).bfloat16()
                for w in range(tt_output_tensor.shape[0]):
                    for z in range(tt_output_tensor.shape[1]):
                        for y in range(0, tt_output_tensor.shape[2], 32):
                            for x in range(0, tt_output_tensor.shape[3], 32):
                                if tt_output_tensor[w, z, y, x] != golden_output_tensors[i][w, z, y, x]:
                                    mismatch_tensor[w, z, y // 32, x // 32] = 1
                                    logger.error(
                                        f"mismatch at {w}, {z}, {y}, {x}: {tt_output_tensor[w, z, y, x]} != {golden_output_tensors[i][w, z, y, x]}"
                                    )
                logger.error(f"MISMATCH TENSOR {mismatch_tensor}")

        else:
            logger.info(f"output match for tensor {i}")
    assert not mismatch, f"{i} FAILED: {output}"


# ~2:45 extra time in the current state
@skip_for_grayskull("Requires eth connected devices to run")
@pytest.mark.timeout(120)
@pytest.mark.parametrize(
    "num_devices, num_links",
    [
        (4, 1),
    ],
)
@pytest.mark.parametrize(
    "per_chip_output_shape, dim, layout",
    [
        # ([1, 1, 32, 32], 3, ttnn.TILE_LAYOUT),
        # ([1, 1, 32, 32], 3, ttnn.TILE_LAYOUT),
        # ([1, 1, 32, 32 * 2], 3, ttnn.TILE_LAYOUT),
        # ([1, 1, 64, 32], 3, ttnn.TILE_LAYOUT),
        # ([1, 1, 64, 64], 3, ttnn.TILE_LAYOUT),
        # ([1, 1, 128, 128], 0, ttnn.TILE_LAYOUT),
        # ([1, 1, 128, 128], 1, ttnn.TILE_LAYOUT),
        # ([1, 1, 128, 128], 2, ttnn.TILE_LAYOUT),
        # ([1, 1, 128, 128], 3, ttnn.TILE_LAYOUT),
        # ([1, 1, 32, 32], 2, ttnn.TILE_LAYOUT),
        # ([1, 1, 32, 64], 2, ttnn.TILE_LAYOUT),
        # ([1, 1, 32, 32 * 4], 3, ttnn.TILE_LAYOUT),
        # ([1, 1, 128, 4096], 3, ttnn.TILE_LAYOUT),
        ([1, 4, 32, 2304], 2, ttnn.TILE_LAYOUT),
        # ([1, 2, 224, 32 * 8], 3, ttnn.TILE_LAYOUT),
        # ([1, 8, 1024, 1024], 3, ttnn.TILE_LAYOUT),
        # ([1, 4, 2048, 1024], 3, ttnn.TILE_LAYOUT),
        # ([1, 1, 128, 8192], 3, ttnn.TILE_LAYOUT),
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
@pytest.mark.parametrize("math_op", [ttnn.ReduceType.Sum])
@pytest.mark.parametrize("enable_async", [False])
@pytest.mark.parametrize("trace_mode", [False])
@pytest.mark.parametrize("device_params", [{"trace_region_size": 27648}], indirect=True)
def test_line_reduce_scatter_async_post_commit(
    t3k_mesh_device,
    num_devices,
    per_chip_output_shape,
    dim,
    num_links,
    math_op,
    input_dtype,
    layout,
    mem_config,
    use_program_cache,
    function_level_defaults,
    enable_async,
    trace_mode,
    num_iters=1,
):
    run_reduce_scatter_test(
        t3k_mesh_device,
        num_devices,
        per_chip_output_shape,
        dim,
        num_links,
        math_op,
        input_dtype,
        layout,
        mem_config,
        use_program_cache,
        function_level_defaults,
        num_iters=num_iters,
        enable_async=enable_async,
        topology=ttnn.Topology.Linear,
        trace_mode=trace_mode,
    )