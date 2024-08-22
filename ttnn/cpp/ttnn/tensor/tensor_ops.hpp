// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
//
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "types.hpp"

namespace tt::tt_metal {
struct Tensor;
class CommandQueue;
struct MemoryConfig;
class Device;
class DeviceMesh;
}

namespace tt::tt_metal::tensor_ops {

Tensor tensor_to(const Tensor& input_tensor, Device* target_device, const MemoryConfig& mem_config);

Tensor tensor_to(const Tensor& input_tensor, const std::vector<Device*>& workers, const MemoryConfig& mem_config);

Tensor tensor_to(const Tensor& input_tensor, Layout target_layout, Device* worker);

Tensor tensor_to(const Tensor& input_tensor, Layout target_layout, DeviceMesh* device_mesh);

Tensor tensor_cpu(const Tensor& input_tensor, bool blocking, uint8_t cq_id);

Tensor tensor_cpu_sharded(const Tensor& input_tensor);

void tensor_print(const Tensor& input_tensor);

Tensor tensor_pad(const Tensor& input_tensor, const Shape& output_tensor_shape, const Shape& input_tensor_start, float pad_value);

Tensor tensor_unpad(const Tensor& input_tensor, const Shape& output_tensor_start, const Shape& output_tensor_end);

Tensor tensor_pad_to_tile(const Tensor& input_tensor, float pad_value);

Tensor tensor_unpad_from_tile(const Tensor& input_tensor, const Shape& output_tensor_shape);

Tensor tensor_reshape(const Tensor& input_tensor, int N, int C, int H, int W);

Tensor tensor_reshape(const Tensor& input_tensor, const Shape& new_shape);

}