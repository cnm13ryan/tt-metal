// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
//
// SPDX-License-Identifier: Apache-2.0

#include "ttnn/tensor/tensor_utils.hpp"

#include "ttnn/distributed/api.hpp"
#include "ttnn/tensor/host_buffer/functions.hpp"
#include "ttnn/tensor/types.hpp"

namespace tt {

namespace tt_metal {

template <typename T, typename compute_>
Tensor convert_tensor(const Tensor& input_tensor, compute_& compute) {
    auto convert_tensor = [&compute](const auto& input_tensor) {
        return std::visit(
            [&compute](auto&& storage) -> Tensor {
                using StorageType = std::decay_t<decltype(storage)>;
                if constexpr (std::is_same_v<StorageType, OwnedStorage>) {
                    return compute(owned_buffer::get_as<T>(storage.buffer));
                } else if constexpr (std::is_same_v<StorageType, BorrowedStorage>) {
                    return compute(borrowed_buffer::get_as<T>(storage.buffer));
                } else {
                    TT_THROW("Unsupported storage type");
                }
            },
            input_tensor.get_storage());
    };

    return ttnn::distributed::is_multi_device_tensor(input_tensor) ? transform(input_tensor, convert_tensor)
                                                                   : convert_tensor(input_tensor);
}
template <typename Func, typename... Args>
Tensor convert_tensor_to_tiled_layout_common(
    const Tensor& input_tensor,
    std::optional<DataType> output_dtype,
    const std::unordered_map<DataType, Func>& function_map,
    Args&&... args) {
    TT_ASSERT(
        input_tensor.get_layout() == Layout::ROW_MAJOR &&
        "Tensor(weight/bias) should be in row major layout for conversion to tilized layout.");

    if (output_dtype.has_value()) {
        if (output_dtype == DataType::BFLOAT8_B || output_dtype == DataType::BFLOAT4_B) {
            TT_ASSERT(input_tensor.get_dtype() == DataType::FLOAT32);
        } else {
            TT_ASSERT(input_tensor.get_dtype() == input_tensor.get_dtype());
        }
    }
    auto entry = function_map.find(input_tensor.get_dtype());
    if (entry == function_map.end()) {
        TT_THROW("Unsupported data type");
    }
    return entry->second(input_tensor, std::forward<Args>(args)..., output_dtype.value_or(input_tensor.get_dtype()));
}

template <typename T>
Tensor create_tensor_from_owned_buffer(
    owned_buffer::Buffer<T>& buf, DataType& output_dtype, ttnn::SimpleShape& output_shape) {
    if constexpr (std::is_same<T, float>::value) {
        if (output_dtype == DataType::BFLOAT8_B || output_dtype == DataType::BFLOAT4_B) {
            auto tensor =
                Tensor(std::move(OwnedStorage{std::move(buf)}), output_shape, DataType::FLOAT32, Layout::ROW_MAJOR)
                    .to(Layout::TILE);
            auto output_float_data = owned_buffer::get_as<float>(tensor).get();
            auto output_packed_data =
                output_dtype == DataType::BFLOAT8_B
                    ? pack_fp32_vec_as_bfp8_tiles(output_float_data, /*row_major_input=*/false, /*is_exp_a=*/false)
                    : pack_fp32_vec_as_bfp4_tiles(output_float_data, /*row_major_input=*/false, /*is_exp_a=*/false);
            auto output_uint32_buffer = owned_buffer::create<uint32_t>(std::move(output_packed_data));
            return Tensor(
                std::move(OwnedStorage{std::move(output_uint32_buffer)}), output_shape, output_dtype, Layout::TILE);
        }
    } else {
        TT_FATAL(
            (output_dtype != DataType::BFLOAT8_B) || (output_dtype != DataType::BFLOAT4_B),
            "Unsupported output datatype");
    }
    auto rm_tensor = Tensor(std::move(OwnedStorage{std::move(buf)}), output_shape, output_dtype, Layout::ROW_MAJOR);
    return rm_tensor.to(Layout::TILE);
}

template <typename T>
Tensor to_weight_special_padding_tile_layout(
    const Tensor& conv_weight_tensor, uint32_t in1_block_h, uint32_t in1_block_w, DataType output_dtype) {
    auto w_shape = conv_weight_tensor.get_legacy_shape();
    auto compute = [&w_shape, &in1_block_h, &in1_block_w, &output_dtype](const auto& input_buffer) {
        uint32_t in1_block_h_datums = in1_block_h * constants::TILE_HEIGHT;
        uint32_t in1_block_w_datums = in1_block_w * constants::TILE_WIDTH;
        auto weight_matrix_cols = w_shape[0];
        // width padding
        if (weight_matrix_cols % in1_block_w_datums != 0) {
            weight_matrix_cols =
                (uint32_t)std::ceil((double)weight_matrix_cols / (double)in1_block_w_datums) * in1_block_w_datums;
        }
        // height padding
        assert(in1_block_h_datums >= w_shape[1] * w_shape[3]);
        uint32_t block_height_padding = in1_block_h_datums - (w_shape[1] * w_shape[3]);
        auto weight_matrix_rows = ((w_shape[1] * w_shape[3]) + block_height_padding) * w_shape[2];
        ttnn::SimpleShape output_shape{1, 1, weight_matrix_rows, weight_matrix_cols};
        auto output_buffer = owned_buffer::create<T>(output_shape.volume());
        for (auto r = 0; r < w_shape[2]; r++) {
            for (auto s = 0; s < w_shape[3]; s++) {
                for (auto c = 0; c < w_shape[1]; c++) {
                    for (auto k = 0; k < w_shape[0]; k++) {
                        auto matrix_idx = k + c * weight_matrix_cols + s * w_shape[1] * weight_matrix_cols +
                                          r * ((w_shape[3] * w_shape[1]) + block_height_padding) * weight_matrix_cols;
                        auto idx =
                            k * w_shape[1] * w_shape[2] * w_shape[3] + c * w_shape[2] * w_shape[3] + r * w_shape[3] + s;
                        output_buffer[matrix_idx] = input_buffer[idx];
                    }
                }
            }
        }
        return create_tensor_from_owned_buffer<T>(output_buffer, output_dtype, output_shape);
    };
    return convert_tensor<T>(conv_weight_tensor, compute);
}

template <typename T>
Tensor to_weight_tile_layout(
    const Tensor& conv_weight_tensor, uint32_t in1_block_h, uint32_t in1_block_w, DataType output_dtype) {
    auto w_shape = conv_weight_tensor.get_legacy_shape();
    auto compute = [&w_shape, &in1_block_h, &in1_block_w, &output_dtype](const auto& input_buffer) {
        auto weight_matrix_cols = w_shape[0];
        // width padding
        uint32_t in1_block_w_datums = in1_block_w * constants::TILE_WIDTH;
        if (weight_matrix_cols % in1_block_w_datums != 0) {
            weight_matrix_cols =
                (uint32_t)std::ceil((double)weight_matrix_cols / (double)in1_block_w_datums) * in1_block_w_datums;
        }
        // height padding
        auto weight_matrix_rows = w_shape[1] * w_shape[2] * w_shape[3];
        uint32_t in1_block_h_datums = in1_block_h * constants::TILE_HEIGHT;
        if (weight_matrix_rows % in1_block_h_datums != 0) {
            weight_matrix_rows =
                (uint32_t)std::ceil((double)weight_matrix_rows / (double)in1_block_h_datums) * in1_block_h_datums;
        }
        ttnn::SimpleShape output_shape{1, 1, weight_matrix_rows, weight_matrix_cols};
        auto output_buffer = owned_buffer::create<T>(output_shape.volume());
        for (auto r = 0; r < w_shape[2]; r++) {
            for (auto s = 0; s < w_shape[3]; s++) {
                for (auto c = 0; c < w_shape[1]; c++) {
                    for (auto k = 0; k < w_shape[0]; k++) {
                        auto matrix_idx = k + c * weight_matrix_cols + s * w_shape[1] * weight_matrix_cols +
                                          r * w_shape[3] * w_shape[1] * weight_matrix_cols;
                        auto idx =
                            k * w_shape[1] * w_shape[2] * w_shape[3] + c * w_shape[2] * w_shape[3] + r * w_shape[3] + s;
                        output_buffer[matrix_idx] = input_buffer[idx];
                    }
                }
            }
        }
        return create_tensor_from_owned_buffer<T>(output_buffer, output_dtype, output_shape);
    };

    return convert_tensor<T>(conv_weight_tensor, compute);
}

// Converts convolution weights to tilized 2d matrix layout.
// Returns a new tensor with layout=Tile
Tensor convert_conv_weight_tensor_to_tiled_layout(
    const Tensor& conv_weight_tensor,
    uint32_t in1_block_h,
    uint32_t in1_block_w,
    std::optional<DataType> output_dtype) {
    const static std::unordered_map<DataType, std::function<Tensor(const Tensor&, uint32_t, uint32_t, DataType)>>
        to_w_tile_layout_map = {
            {DataType::BFLOAT16, &to_weight_tile_layout<bfloat16>},
            {DataType::FLOAT32, &to_weight_tile_layout<float>},
            {DataType::UINT32, &to_weight_tile_layout<uint32_t>}};

    return convert_tensor_to_tiled_layout_common(
        conv_weight_tensor, output_dtype, to_w_tile_layout_map, in1_block_h, in1_block_w);
}

template <typename T>
Tensor to_weight_tile_layout_block_sharded(
    const Tensor& conv_weight_tensor, uint32_t num_channel_shards, DataType output_dtype) {
    auto w_shape = conv_weight_tensor.get_legacy_shape();
    auto compute = [&w_shape, &num_channel_shards, &output_dtype](const auto& input_buffer) {
        auto weight_matrix_cols = w_shape[0];
        TT_ASSERT(weight_matrix_cols % num_channel_shards == 0);
        auto conv_output_shard_width = weight_matrix_cols / num_channel_shards;
        auto conv_output_shard_width_padded =
            (uint32_t)std::ceil((double)conv_output_shard_width / (double)constants::TILE_WIDTH) *
            constants::TILE_WIDTH;
        if (conv_output_shard_width < conv_output_shard_width_padded) {
            // width padding for conv output shard padding
            weight_matrix_cols = conv_output_shard_width_padded * num_channel_shards;
        }

        auto weight_matrix_rows = w_shape[1] * w_shape[2] * w_shape[3];
        TT_ASSERT(w_shape[1] % num_channel_shards == 0);
        auto conv_input_shard_width = w_shape[1] / num_channel_shards;
        auto weight_block_height = conv_input_shard_width * w_shape[2] * w_shape[3];
        auto weight_block_height_padded =
            (uint32_t)std::ceil((double)weight_block_height / (double)constants::TILE_HEIGHT) * constants::TILE_HEIGHT;
        if (weight_block_height < weight_block_height_padded) {
            // height padding for non tile multiple block height
            weight_matrix_rows = weight_block_height_padded * num_channel_shards;
        }
        ttnn::SimpleShape output_shape{1, 1, weight_matrix_rows, weight_matrix_cols};
        auto output_buffer = owned_buffer::create<T>(output_shape.volume());
        for (auto ic = 0; ic < num_channel_shards; ic++) {
            for (auto r = 0; r < w_shape[2]; r++) {
                for (auto s = 0; s < w_shape[3]; s++) {
                    for (auto c_s = 0; c_s < conv_input_shard_width; c_s++) {
                        for (auto oc = 0; oc < num_channel_shards; oc++) {
                            for (auto k_s = 0; k_s < conv_output_shard_width; k_s++) {
                                auto matrix_idx = (oc * conv_output_shard_width_padded + k_s) +
                                                  c_s * weight_matrix_cols +
                                                  s * conv_input_shard_width * weight_matrix_cols +
                                                  r * w_shape[3] * conv_input_shard_width * weight_matrix_cols +
                                                  ic * weight_block_height_padded * weight_matrix_cols;
                                auto idx = (oc * conv_output_shard_width + k_s) * w_shape[1] * w_shape[2] * w_shape[3] +
                                           (ic * conv_input_shard_width + c_s) * w_shape[2] * w_shape[3] +
                                           r * w_shape[3] + s;
                                output_buffer[matrix_idx] = input_buffer[idx];
                            }
                        }
                    }
                }
            }
        }
        return create_tensor_from_owned_buffer<T>(output_buffer, output_dtype, output_shape);
    };
    return convert_tensor<T>(conv_weight_tensor, compute);
}

// Converts convolution weights to tilized 2d matrix layout for block sharded conv.
// Returns a new tensor with layout=Tile
Tensor convert_conv_weight_tensor_to_tiled_layout_block_sharded(
    const Tensor& conv_weight_tensor, uint32_t num_channel_shards, std::optional<DataType> output_dtype) {
    const static std::unordered_map<DataType, std::function<Tensor(const Tensor&, uint32_t, DataType)>>
        to_w_tile_layout_map = {
            {DataType::BFLOAT16, &to_weight_tile_layout_block_sharded<bfloat16>},
            {DataType::FLOAT32, &to_weight_tile_layout_block_sharded<float>},
            {DataType::UINT32, &to_weight_tile_layout_block_sharded<uint32_t>}};

    return convert_tensor_to_tiled_layout_common(
        conv_weight_tensor, output_dtype, to_w_tile_layout_map, num_channel_shards);
}

template <typename T>
Tensor to_bias_tile_layout_block_sharded(
    const Tensor& conv_bias_tensor, uint32_t num_channel_shards, DataType output_dtype) {
    auto b_shape = conv_bias_tensor.get_legacy_shape();
    TT_ASSERT(b_shape[0] == 1 && b_shape[1] == 1 && b_shape[2] == 1);
    auto compute = [&b_shape, &num_channel_shards, &output_dtype](const auto& input_buffer) {
        auto bias_matrix_cols = b_shape[3];
        /*TT_ASSERT(bias_matrix_cols % num_channel_shards == 0);*/
        auto conv_output_shard_width = bias_matrix_cols / num_channel_shards;
        auto conv_output_shard_width_padded =
            (uint32_t)std::ceil((double)conv_output_shard_width / (double)constants::TILE_WIDTH) *
            constants::TILE_WIDTH;
        if (conv_output_shard_width < conv_output_shard_width_padded) {
            // width padding for conv output shard padding
            bias_matrix_cols = conv_output_shard_width_padded * num_channel_shards;
        }

        auto bias_matrix_rows = 32;
        ttnn::SimpleShape output_shape{1, 1, bias_matrix_rows, bias_matrix_cols};
        auto output_buffer = owned_buffer::create<T>(output_shape.volume());
        for (auto oc = 0; oc < num_channel_shards; oc++) {
            for (auto k_s = 0; k_s < conv_output_shard_width; k_s++) {
                auto matrix_idx = oc * conv_output_shard_width_padded + k_s;
                auto idx = oc * conv_output_shard_width + k_s;
                output_buffer[matrix_idx] = input_buffer[idx];
            }
        }
        return create_tensor_from_owned_buffer<T>(output_buffer, output_dtype, output_shape);
    };

    return convert_tensor<T>(conv_bias_tensor, compute);
}

// Converts convolution bias to tilized 2d matrix layout for block sharded conv.
// Returns a new tensor with layout=Tile
Tensor convert_conv_bias_tensor_to_tiled_layout_block_sharded(
    const Tensor& conv_bias_tensor, uint32_t num_channel_shards, std::optional<DataType> output_dtype) {
    const static std::unordered_map<
        DataType,
        std::function<Tensor(const Tensor&, uint32_t num_channel_shards, DataType output_dtype)>>
        to_b_tile_layout_map = {
            {DataType::BFLOAT16, &to_bias_tile_layout_block_sharded<bfloat16>},
            {DataType::FLOAT32, &to_bias_tile_layout_block_sharded<float>},
            {DataType::UINT32, &to_bias_tile_layout_block_sharded<uint32_t>},
        };
    return convert_tensor_to_tiled_layout_common(
        conv_bias_tensor, output_dtype, to_b_tile_layout_map, num_channel_shards);
}

// Converts convolution weights to tilized 2d matrix layout.
// Returns a new tensor with layout=Tile
Tensor convert_conv_weight_tensor_to_special_padding_tiled_layout(
    const Tensor& conv_weight_tensor,
    uint32_t in1_block_h,
    uint32_t in1_block_w,
    std::optional<DataType> output_dtype) {
    const static std::unordered_map<DataType, std::function<Tensor(const Tensor&, uint32_t, uint32_t, DataType)>>
        to_w_tile_layout_map = {
            {DataType::BFLOAT16, &to_weight_special_padding_tile_layout<bfloat16>},
            {DataType::FLOAT32, &to_weight_special_padding_tile_layout<float>},
            {DataType::UINT32, &to_weight_special_padding_tile_layout<uint32_t>}};

    return convert_tensor_to_tiled_layout_common(
        conv_weight_tensor, output_dtype, to_w_tile_layout_map, in1_block_h, in1_block_w);
}

/*
Helper function to aid in converting grouped weight tensor to ungrouped weight tensor with padded zero channels
*/
template <typename T>
static Tensor conv_group_weight_zero_pad_helper(
    const Tensor& weight,
    const ttnn::SimpleShape& original_weight_shape,
    const ttnn::SimpleShape& output_weight_shape,
    uint32_t num_groups,
    DataType output_dtype) {
    auto pad_weight = [&original_weight_shape, &output_weight_shape, &num_groups, &output_dtype](
                          const auto& conv_weight_tensor_buffer) {
        owned_buffer::Buffer<T> output_buffer = owned_buffer::create<T>(output_weight_shape.volume());
        for (int curr_batch_idx = 0; curr_batch_idx < original_weight_shape[0]; curr_batch_idx++) {
            int new_batch_idx = curr_batch_idx;

            // Find which group_id the filter belongs to - through this, we can compute the offset where the padding
            // should be applied
            auto group_size = original_weight_shape[0] / num_groups;
            auto group_index = curr_batch_idx / group_size;
            auto group_id = std::min(group_index, num_groups - 1);
            int new_channel_start_idx = group_id * original_weight_shape[1];

            for (int j = 0; j < original_weight_shape[1]; j++) {
                for (int k = 0; k < original_weight_shape[2]; k++) {
                    for (int m = 0; m < original_weight_shape[3]; m++) {
                        // Get value from original weight tensor
                        auto value_flat_input_index = compute_flat_indices(
                            ttnn::SmallVector<int>{curr_batch_idx, j, k, m}, compute_strides(original_weight_shape));
                        auto value = conv_weight_tensor_buffer[value_flat_input_index];

                        // Copy value to output tensor at the adjusted position
                        auto new_channel_idx = new_channel_start_idx + j;
                        auto output_flat_input_index = compute_flat_indices(
                            ttnn::SmallVector<int>{new_batch_idx, new_channel_idx, k, m},
                            compute_strides(output_weight_shape));
                        output_buffer[output_flat_input_index] = value;
                    }
                }
            }
        }
        return Tensor(
            std::move(OwnedStorage{std::move(output_buffer)}), output_weight_shape, output_dtype, Layout::ROW_MAJOR);
    };

    return convert_tensor<T>(weight, pad_weight);
}

/*
Helper function to aid in converting depthwise weight tensor to broadcasted weight tensor with repeated input channels
*/
template <typename T>
static Tensor conv_depthwise_weight_bcast_helper(
    const Tensor& conv_weight_tensor,
    const ttnn::SimpleShape& original_weight_shape,
    const ttnn::SimpleShape& output_weight_shape,
    DataType output_dtype) {
    owned_buffer::Buffer<T> output_buffer = owned_buffer::create<T>(output_weight_shape.volume());
    auto conv_weight_tensor_buffer = borrowed_buffer::get_as<T>(conv_weight_tensor);
    // Copy the original weight tensor to the output tensor
    for (int i = 0; i < output_weight_shape[0]; i++) {
        for (int j = 0; j < output_weight_shape[1]; j++) {
            for (int k = 0; k < output_weight_shape[2]; k++) {
                for (int l = 0; l < output_weight_shape[3]; l++) {
                    auto value_flat_input_index = compute_flat_indices(
                        ttnn::SmallVector<int>{i, 0, k, l}, compute_strides(original_weight_shape));
                    auto value = conv_weight_tensor_buffer[value_flat_input_index];
                    auto output_flat_input_index =
                        compute_flat_indices(ttnn::SmallVector<int>{i, j, k, l}, compute_strides(output_weight_shape));
                    output_buffer[output_flat_input_index] = value;
                }
            }
        }
    }

    auto output_tensor =
        Tensor(std::move(OwnedStorage{std::move(output_buffer)}), output_weight_shape, output_dtype, Layout::ROW_MAJOR);
    return output_tensor;
}

/*
Converts convolution weights to grouped layout with padded zeros
This function will take in a weight tensor with shape [out_channels, in_channels // groups, H, W] and return a newly
allocated output tensor with shape [out_channels, in_channels, H, W] The extra channels in shape[1] will be padded with
0 - then the entire weight tensor is convolved with the input tensor - equivalent to convolution if the input tensor was
divided into num_groups for each groupped filter
*/
Tensor convert_conv_weight_tensor_to_grouped_layout(
    const Tensor& conv_weight_tensor, uint32_t num_groups, DataType output_dtype) {
    // Define output tensor shape. This is going to be channel dimension of weight tensor * num_groups - this value
    // should match number of input channels being convolved with the weight tensor
    auto original_conv_weight_tensor_shape_test = conv_weight_tensor.get_shape();
    ttnn::SimpleShape original_conv_weight_tensor_shape{
        original_conv_weight_tensor_shape_test[0],
        original_conv_weight_tensor_shape_test[1],
        original_conv_weight_tensor_shape_test[2],
        original_conv_weight_tensor_shape_test[3]};
    ttnn::SimpleShape output_conv_weight_tensor_shape{
        original_conv_weight_tensor_shape[0],
        original_conv_weight_tensor_shape[1] * num_groups,
        original_conv_weight_tensor_shape[2],
        original_conv_weight_tensor_shape[3]};

    const static std::unordered_map<
        DataType,
        std::function<Tensor(const Tensor&, ttnn::SimpleShape, ttnn::SimpleShape, uint32_t, DataType)>>
        to_w_tile_layout_map = {
            {DataType::INT32, &conv_group_weight_zero_pad_helper<int32_t>},
            {DataType::FLOAT32, &conv_group_weight_zero_pad_helper<float>},
            {DataType::BFLOAT16, &conv_group_weight_zero_pad_helper<bfloat16>},
            {DataType::UINT16, &conv_group_weight_zero_pad_helper<uint16_t>},
            {DataType::BFLOAT8_B, &conv_group_weight_zero_pad_helper<float>},
            {DataType::UINT32, &conv_group_weight_zero_pad_helper<uint32_t>},
            {DataType::BFLOAT4_B, &conv_group_weight_zero_pad_helper<uint32_t>},
        };
    output_dtype = output_dtype == DataType::BFLOAT8_B ? DataType::FLOAT32 : output_dtype;

    return convert_tensor_to_tiled_layout_common(
        conv_weight_tensor,
        output_dtype,
        to_w_tile_layout_map,
        original_conv_weight_tensor_shape,
        output_conv_weight_tensor_shape,
        num_groups);
}

/*
Converts convolution weights to depthwise layout
This function will take in a weight tensor with shape [out_channels, 1, H, W] and return a newly
allocated output tensor with shape [out_channels, act_block_h, H, W] The extra channels in shape[1] are repeated
from the original weight tensor - it would be convolving act_block in conv_matrix in one go
*/
Tensor convert_conv_weight_tensor_to_depthwise_layout(
    const Tensor& conv_weight_tensor, uint32_t act_block_h_ntiles, DataType output_dtype) {
    auto original_conv_weight_tensor_shape_test = conv_weight_tensor.get_shape();
    uint32_t num_input_channels_to_repeat = act_block_h_ntiles * constants::TILE_HEIGHT;
    ttnn::SimpleShape original_conv_weight_tensor_shape{
        original_conv_weight_tensor_shape_test[0],
        original_conv_weight_tensor_shape_test[1],
        original_conv_weight_tensor_shape_test[2],
        original_conv_weight_tensor_shape_test[3]};
    ttnn::SimpleShape output_conv_weight_tensor_shape{
        original_conv_weight_tensor_shape[0],
        num_input_channels_to_repeat,
        original_conv_weight_tensor_shape[2],
        original_conv_weight_tensor_shape[3]};

    // Create newly allocated buffer all initialized to 0 depending on the datatype of the weight tensor
    const static std::
        unordered_map<DataType, std::function<Tensor(const Tensor&, ttnn::SimpleShape, ttnn::SimpleShape, DataType)>>
            to_w_tile_layout_map = {
                {DataType::INT32, &conv_depthwise_weight_bcast_helper<int32_t>},
                {DataType::FLOAT32, &conv_depthwise_weight_bcast_helper<float>},
                {DataType::BFLOAT16, &conv_depthwise_weight_bcast_helper<bfloat16>},
                {DataType::UINT16, &conv_depthwise_weight_bcast_helper<uint16_t>},
                {DataType::BFLOAT8_B, &conv_depthwise_weight_bcast_helper<float>},
                {DataType::UINT32, &conv_depthwise_weight_bcast_helper<uint32_t>},
                {DataType::BFLOAT4_B, &conv_depthwise_weight_bcast_helper<uint32_t>},
            };
    output_dtype = ((output_dtype == DataType::BFLOAT8_B) || (output_dtype == DataType::BFLOAT4_B)) ? DataType::FLOAT32
                                                                                                    : output_dtype;

    return convert_tensor_to_tiled_layout_common(
        conv_weight_tensor,
        output_dtype,
        to_w_tile_layout_map,
        original_conv_weight_tensor_shape,
        output_conv_weight_tensor_shape);
}

const ttnn::SimpleShape infer_dims_for_reshape(const Tensor& tensor, tt::stl::Span<const int32_t> shape) {
    int64_t old_volume = tensor.get_logical_volume();
    int64_t new_volume = 1;
    int64_t index_of_negative_1 = -1;
    bool has_zero = false;
    for (auto index = 0; index < shape.size(); ++index) {
        if (shape[index] == -1) {
            if (index_of_negative_1 != -1) {
                std::string error_msg = "Shape cannot have more than 1 elements that is set to -1! Shape used: (";
                for (auto& s : shape) {
                    error_msg += std::to_string(s) + ",";
                }
                error_msg += ")";
                TT_THROW("{}", error_msg);
            }
            index_of_negative_1 = index;
        } else {
            if (shape[index] == 0) {
                has_zero = true;
            }
            new_volume *= shape[index];
        }
    }
    if (has_zero && index_of_negative_1 != -1) {
        std::string error_msg = "cannot reshape tensor of 0 elements into shape (";
        for(auto & s: shape) {
            error_msg += std::to_string(s) + ",";
        }
        error_msg += ") because the unspecified dimension size -1 can be any value and is ambiguous";
        TT_THROW("{}", error_msg);
    }

    ttnn::SmallVector<uint32_t> new_shape(shape.size());
    std::copy(shape.begin(), shape.end(), new_shape.begin());
    if (index_of_negative_1 == -1) {
        TT_FATAL(new_volume == old_volume, "Invalid arguments to reshape");
    } else {
        TT_FATAL(old_volume % new_volume == 0, "Invalid arguments to reshape");
        new_shape[index_of_negative_1] = old_volume / new_volume;
    }

    return ttnn::SimpleShape(std::move(new_shape));
}

bool is_arch_gs(const tt::ARCH& arch) { return arch == tt::ARCH::GRAYSKULL; }

bool is_arch_whb0(const tt::ARCH& arch) { return arch == tt::ARCH::WORMHOLE_B0; }

bool is_cpu_tensor(const Tensor& tensor) {
    return tensor.storage_type() == StorageType::OWNED || tensor.storage_type() == StorageType::BORROWED;
}

bool is_device_tensor(const Tensor& tensor) { return tensor.storage_type() == StorageType::DEVICE; }

Tensor transform(const Tensor& tensor, std::function<Tensor(const Tensor&)> transform_func) {
    auto input_tensors = ttnn::distributed::get_tensors_from_multi_device_storage(tensor);
    std::vector<Tensor> output_tensors(input_tensors.size());
    std::transform(input_tensors.begin(), input_tensors.end(), output_tensors.begin(), [&](const auto& device_tensor) {
        return transform_func(device_tensor);
    });
    return ttnn::distributed::create_multi_device_tensor(
        output_tensors, tensor.storage_type(), ttnn::distributed::get_distributed_tensor_config_from_tensor(tensor));
}

void apply(const Tensor& tensor, const std::function<void(const Tensor&)>& callable) {
    auto input_tensors = ttnn::distributed::get_tensors_from_multi_device_storage(tensor);
    for (const auto& device_tensor : input_tensors) {
        callable(device_tensor);
    }
}

std::vector<Device*> get_devices(const Tensor& tensor) {
    std::vector<Device*> devices;
    if (tensor.storage_type() == tt::tt_metal::StorageType::MULTI_DEVICE) {
        TT_ASSERT(
            std::holds_alternative<tt::tt_metal::MultiDeviceStorage>(tensor.get_storage()),
            "Unexpected type {}",
            tt::stl::get_active_type_name_in_variant(tensor.get_storage()));
        const auto& tensor_storage = std::get<tt::tt_metal::MultiDeviceStorage>(tensor.get_storage());
        for (int i = 0; i < tensor_storage.ordered_device_ids.size(); ++i) {
            auto device_id = tensor_storage.ordered_device_ids[i];
            devices.push_back(tensor_storage.get_buffer_for_device_id(device_id)->device());
        }
        return devices;
    } else {
        TT_THROW("Tensor is not a multi-device tensor");
    }
}

uint32_t num_buffers_in_tensor(const Tensor& tensor) {
    if (std::holds_alternative<MultiDeviceStorage>(tensor.get_storage())) {
        auto device_storage = std::get<tt::tt_metal::MultiDeviceStorage>(tensor.get_storage());
        return device_storage.num_buffers();
    } else if (std::holds_alternative<MultiDeviceHostStorage>(tensor.get_storage())) {
        auto host_storage = std::get<tt::tt_metal::MultiDeviceHostStorage>(tensor.get_storage());
        return host_storage.num_buffers();
    } else if (
        std::holds_alternative<DeviceStorage>(tensor.get_storage()) ||
        std::holds_alternative<OwnedStorage>(tensor.get_storage()) ||
        std::holds_alternative<BorrowedStorage>(tensor.get_storage())) {
        return 1;
    } else {
        TT_THROW("num_buffers_in_tensor only supports multi-device or device tensors");
    }
}

Tensor get_shard_for_device(const Tensor& tensor, Device* target_device, std::optional<int> buffer_index) {
    ZoneScopedN("GetShardForDevice");
    Tensor shard = Tensor();
    auto& storage = tensor.tensor_attributes->storage;
    std::visit(
        [target_device, buffer_index, &tensor, &shard](auto&& s) {
            using T = std::decay_t<decltype(s)>;
            // Stalling reads for tensor data-type and layout are needed here
            // since some worker might have raced ahead to these lookups, while
            // another worker is populating this metadata.
            const Tile tile = tensor.get_tensor_spec().tile();
            if constexpr (std::is_same_v<T, MultiDeviceStorage>) {
                shard = Tensor{
                    DeviceStorage{s.get_buffer_for_device(target_device)},
                    s.get_tensor_shape_for_device(target_device),
                    tensor.get_dtype(),
                    tensor.get_layout(),
                    tile};
            } else if constexpr (std::is_same_v<T, MultiDeviceHostStorage>) {
                shard = Tensor{
                    OwnedStorage{s.get_buffer(buffer_index.value())},
                    s.get_tensor_shape(buffer_index.value()),
                    tensor.get_dtype(),
                    tensor.get_layout(),
                    tile};
            } else if constexpr (
                std::is_same_v<T, OwnedStorage> || std::is_same_v<T, BorrowedStorage> ||
                std::is_same_v<T, DeviceStorage>) {
                shard = tensor;
            } else {
                TT_THROW("get_shard_for_device only supports multi-device or device tensors");
            }
        },
        storage);
    return shard;
}

void insert_buffer_and_shape_for_device(
    Device* target_device, const Tensor& shard, Tensor& tensor_to_modify, std::optional<int> buffer_index) {
    ZoneScopedN("InsertBufferAndShapeForDevice");
    std::visit(
        [target_device, &shard, buffer_index](auto&& s) {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, MultiDeviceHostStorage>) {
                s.insert_buffer_and_shape_for_device(
                    buffer_index.value(),
                    std::get<OwnedStorage>(shard.tensor_attributes->storage).get_buffer(),
                    shard.tensor_attributes->tensor_spec.shape());
            } else if constexpr (std::is_same_v<T, MultiDeviceStorage>) {
                s.insert_buffer_and_shape_for_device(
                    target_device,
                    std::get<DeviceStorage>(shard.tensor_attributes->storage).get_buffer(),
                    shard.tensor_attributes->tensor_spec.shape());
            } else if constexpr (std::is_same_v<T, OwnedStorage>) {
                s.insert_buffer(std::get<OwnedStorage>(shard.tensor_attributes->storage).get_buffer());
            } else if constexpr (std::is_same_v<T, DeviceStorage>) {
                s.insert_buffer(std::get<DeviceStorage>(shard.tensor_attributes->storage).get_buffer());
            } else {
                TT_THROW("Unsupported storage in insert_buffer_and_shape_for_device");
            }
        },
        tensor_to_modify.tensor_attributes->storage);
}

Tensor copy_borrowed_tensor_in_async_mode(Device* worker, const Tensor& tensor) {
    // When using async mode, tensors with borrowed storage cannot be passed to workers.
    // They need to be copied to owned storage before being passed to the worker.
    ZoneScopedN("ConvertBorrowedToOwned");
    // Tensor has workers (on device) or runtime mode is synchronous or tensor has multiple buffers.
    // No need to check for borrowed storage.
    if (worker->get_worker_mode() == WorkExecutorMode::SYNCHRONOUS or
        tensor.tensor_attributes->num_shards_to_be_populated > 1) {
        return tensor;
    }

    if (tensor.storage_type() == StorageType::BORROWED) {
        ZoneScopedN("CopyBorrowedStorage");
        auto borrowed_buffer = std::get<BorrowedStorage>(tensor.get_storage()).buffer;
        Tensor owned_tensor;
        std::visit(
            [&owned_tensor, &tensor](auto&& buffer) {
                using BorrowedStorageType = std::vector<std::decay_t<decltype(*(buffer.begin()))>>;
                auto owned_buf = owned_buffer::create(BorrowedStorageType(buffer.begin(), buffer.end()));
                owned_tensor = Tensor(OwnedStorage{owned_buf}, tensor.get_tensor_spec());
            },
            borrowed_buffer);
        return owned_tensor;
    }
    return tensor;
}

Size get_2d_shape(const ttnn::SimpleShape& shape) {
    size_t width = shape[-1];
    size_t height = shape.volume() / width;
    return Size{height, width};
}

ShardDivisionSpec compute_shard_division_spec(const Size& shape, const Size& shard_shape) {
    const auto num_shards_height = tt::div_up(shape.height(), shard_shape.height());
    const auto last_shard_height =
        shape.height() % shard_shape.height() > 0 ? shape.height() % shard_shape.height() : shard_shape.height();
    const auto num_shards_width = tt::div_up(shape.width(), shard_shape.width());
    const auto last_shard_width =
        shape.width() % shard_shape.width() > 0 ? shape.width() % shard_shape.width() : shard_shape.width();

    return ShardDivisionSpec{num_shards_height, last_shard_height, num_shards_width, last_shard_width};
};

}  // namespace tt_metal

}  // namespace tt
