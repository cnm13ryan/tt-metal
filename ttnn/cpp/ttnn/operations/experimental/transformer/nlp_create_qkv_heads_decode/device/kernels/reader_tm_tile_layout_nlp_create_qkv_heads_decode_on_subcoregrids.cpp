// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
//
// SPDX-License-Identifier: Apache-2.0

#include <stdint.h>
#include "dataflow_api.h"
#include "tt_metal/common/constants.hpp"

using namespace tt::constants;
void kernel_main() {
    uint32_t in_tile_offset_by_batch = get_arg_val<uint32_t>(0);
    uint32_t q_start_addr = get_arg_val<uint32_t>(1);

    constexpr uint32_t ELEMENT_SIZE = get_compile_time_arg_val(0);
    constexpr uint32_t SUBTILE_LINE_BYTES = get_compile_time_arg_val(1);
    constexpr uint32_t cb_id_q_out = get_compile_time_arg_val(2);
    constexpr uint32_t cb_id_k_out = get_compile_time_arg_val(3);
    constexpr uint32_t cb_id_v_out = get_compile_time_arg_val(4);
    constexpr uint32_t head_size = get_compile_time_arg_val(5);
    constexpr uint32_t num_q_heads = get_compile_time_arg_val(6);
    constexpr uint32_t num_kv_heads = get_compile_time_arg_val(7);
    constexpr uint32_t head_size_num_tiles = get_compile_time_arg_val(8);
    constexpr uint32_t PHASES_TO_READ =
        get_compile_time_arg_val(9);  // 0 to read all phases, 1 to read only first phase, 2 to read only second phase
    constexpr uint32_t in_num_cores = get_compile_time_arg_val(10);
    constexpr uint32_t PROCESS_QV = get_compile_time_arg_val(11);
    constexpr uint32_t PROCESS_K = get_compile_time_arg_val(12);

    tt_l1_ptr uint32_t* in0_mcast_noc_x = (tt_l1_ptr uint32_t*)(get_arg_addr(2));
    tt_l1_ptr uint32_t* in0_mcast_noc_y = (tt_l1_ptr uint32_t*)(get_arg_addr(2 + in_num_cores));

    // Q
    uint32_t cur_core_idx = 0;
    uint32_t num_tiles_per_core = head_size_num_tiles * (num_q_heads + 2 * num_kv_heads) / in_num_cores;
    uint32_t num_q_cores = num_tiles_per_core * num_q_heads * head_size_num_tiles;
    uint32_t num_kv_cores = num_tiles_per_core * num_kv_heads * head_size_num_tiles;

    uint64_t qkv_read_addr = get_noc_addr(in0_mcast_noc_x[cur_core_idx], in0_mcast_noc_y[cur_core_idx], q_start_addr) +
                             in_tile_offset_by_batch;
    uint32_t num_tiles_read_cur_core = 0;
    uint32_t q_write_addr = 0;
    constexpr uint32_t tile_size = head_size / head_size_num_tiles;
    constexpr uint32_t HALF_TILE_ELEMENTS = FACE_HEIGHT * TILE_WIDTH;
    constexpr uint32_t SUBTILE_ROWS = FACE_HEIGHT;

    // Skip Q section if PROCESS_QV is False
    if constexpr (PROCESS_QV == 1) {
        for (uint32_t q = 0; q < num_q_heads; ++q) {
            uint32_t wptr_offset = q < SUBTILE_ROWS
                                       ? q * SUBTILE_LINE_BYTES
                                       : (q - SUBTILE_ROWS) * SUBTILE_LINE_BYTES + HALF_TILE_ELEMENTS * ELEMENT_SIZE;
            uint32_t q_write_addr = get_write_ptr(cb_id_q_out) + wptr_offset;
            for (uint32_t i = 0; i < head_size_num_tiles; ++i) {
                // Read first phase
                if constexpr (PHASES_TO_READ == 0 || PHASES_TO_READ == 1) {
                    noc_async_read(qkv_read_addr, q_write_addr, SUBTILE_LINE_BYTES);
                }
                // Read second phase
                if constexpr (PHASES_TO_READ == 0 || PHASES_TO_READ == 2) {
                    noc_async_read(
                        qkv_read_addr + FACE_HW * ELEMENT_SIZE,
                        q_write_addr + FACE_HW * ELEMENT_SIZE,
                        SUBTILE_LINE_BYTES);
                }

                qkv_read_addr += tile_size;
                q_write_addr += tile_size;
                num_tiles_read_cur_core++;

                if (num_tiles_read_cur_core == num_tiles_per_core) {
                    cur_core_idx++;
                    qkv_read_addr =
                        get_noc_addr(in0_mcast_noc_x[cur_core_idx], in0_mcast_noc_y[cur_core_idx], q_start_addr) +
                        in_tile_offset_by_batch;
                    num_tiles_read_cur_core = 0;
                }
            }
        }
    } else {
        cur_core_idx += num_q_cores;
        qkv_read_addr = get_noc_addr(in0_mcast_noc_x[cur_core_idx], in0_mcast_noc_y[cur_core_idx], q_start_addr) +
                        in_tile_offset_by_batch;
    }

    if constexpr (PROCESS_K == 1) {
        // K
        uint32_t k_write_addr = 0;

        // Read 2 phases per tile, where there are num_q_heads * q_num_tiles tiles
        for (uint32_t k = 0; k < num_kv_heads; ++k) {
            uint32_t wptr_offset = k < SUBTILE_ROWS
                                       ? k * SUBTILE_LINE_BYTES
                                       : (k - SUBTILE_ROWS) * SUBTILE_LINE_BYTES + HALF_TILE_ELEMENTS * ELEMENT_SIZE;
            uint32_t k_write_addr = get_write_ptr(cb_id_k_out) + wptr_offset;
            for (uint32_t i = 0; i < head_size_num_tiles; ++i) {
                // Read first phase
                if constexpr (PHASES_TO_READ == 0 || PHASES_TO_READ == 1) {
                    noc_async_read(qkv_read_addr, k_write_addr, SUBTILE_LINE_BYTES);
                }
                // Read second phase
                if constexpr (PHASES_TO_READ == 0 || PHASES_TO_READ == 2) {
                    noc_async_read(
                        qkv_read_addr + FACE_HW * ELEMENT_SIZE,
                        k_write_addr + FACE_HW * ELEMENT_SIZE,
                        SUBTILE_LINE_BYTES);
                }

                qkv_read_addr += tile_size;
                k_write_addr += tile_size;
                num_tiles_read_cur_core++;

                if (num_tiles_read_cur_core == num_tiles_per_core) {
                    cur_core_idx++;
                    qkv_read_addr =
                        get_noc_addr(in0_mcast_noc_x[cur_core_idx], in0_mcast_noc_y[cur_core_idx], q_start_addr) +
                        in_tile_offset_by_batch;
                    num_tiles_read_cur_core = 0;
                }
            }
        }
    } else {
        cur_core_idx += num_kv_cores;
        qkv_read_addr = get_noc_addr(in0_mcast_noc_x[cur_core_idx], in0_mcast_noc_y[cur_core_idx], q_start_addr) +
                        in_tile_offset_by_batch;
    }

    if constexpr (PROCESS_QV == 1) {
        // v
        uint32_t v_write_addr = 0;

        // Read 2 phases per tile, where there are num_q_heads * q_num_tiles tiles
        for (uint32_t v = 0; v < num_kv_heads; ++v) {
            uint32_t wptr_offset = v < SUBTILE_ROWS
                                       ? v * SUBTILE_LINE_BYTES
                                       : (v - SUBTILE_ROWS) * SUBTILE_LINE_BYTES + HALF_TILE_ELEMENTS * ELEMENT_SIZE;
            uint32_t v_write_addr = get_write_ptr(cb_id_v_out) + wptr_offset;
            for (uint32_t i = 0; i < head_size_num_tiles; ++i) {
                // Read first phase
                if constexpr (PHASES_TO_READ == 0 || PHASES_TO_READ == 1) {
                    noc_async_read(qkv_read_addr, v_write_addr, SUBTILE_LINE_BYTES);
                }
                // Read second phase
                if constexpr (PHASES_TO_READ == 0 || PHASES_TO_READ == 2) {
                    noc_async_read(
                        qkv_read_addr + FACE_HW * ELEMENT_SIZE,
                        v_write_addr + FACE_HW * ELEMENT_SIZE,
                        SUBTILE_LINE_BYTES);
                }

                qkv_read_addr += tile_size;
                v_write_addr += tile_size;
                num_tiles_read_cur_core++;

                if (num_tiles_read_cur_core == num_tiles_per_core) {
                    cur_core_idx++;
                    qkv_read_addr =
                        get_noc_addr(in0_mcast_noc_x[cur_core_idx], in0_mcast_noc_y[cur_core_idx], q_start_addr) +
                        in_tile_offset_by_batch;
                    num_tiles_read_cur_core = 0;
                }
            }
        }
    }

    noc_async_read_barrier();
}