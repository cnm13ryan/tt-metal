// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
//
// SPDX-License-Identifier: Apache-2.0

#include "ttnn/run_operation.hpp"
#include "ttnn/operations/core/core.hpp"
#include "ttnn/tensor/tensor.hpp"
#include "ttnn/operations/experimental/reduction/fast_reduce_nc/fast_reduce_nc.hpp"
#include "ttnn/operations/experimental/reduction/fast_reduce_nc/device/fast_reduce_nc_device_operation.hpp"

namespace ttnn {
namespace operations::experimental::reduction{

ttnn::Tensor FastReduceNCOperation::operator()(
    uint8_t queue_id,
    const ttnn::Tensor& input,
    const std::vector<int32_t>& dims,
    const std::optional<const Tensor> output,
    const ttnn::MemoryConfig memory_config,
    std::optional<const DeviceComputeKernelConfig> compute_kernel_config) {
        return detail::fast_reduce_nc(queue_id, input, dims, output, memory_config, compute_kernel_config);
}

ttnn::Tensor FastReduceNCOperation::operator()(
    const ttnn::Tensor& input,
    const std::vector<int32_t>& dims,
    const std::optional<const Tensor> output,
    const ttnn::MemoryConfig memory_config,
    std::optional<const DeviceComputeKernelConfig> compute_kernel_config) {

    return FastReduceNCOperation::operator()(DefaultQueueId, input, dims, output, memory_config, compute_kernel_config);
}


}  // namespace operations::experimental::reduction

}  // namespace ttnn