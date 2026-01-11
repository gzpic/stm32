#include "qwen3_ops.h"

#include <cuda_fp16.h>
#include <cuda_runtime.h>

#include <cmath>

namespace qwen3 {
namespace {

constexpr int kWarpSize = 32;

__device__ __forceinline__ float warp_reduce_sum(float value) {
    for (int mask = kWarpSize / 2; mask > 0; mask >>= 1) {
        value += __shfl_xor_sync(0xffffffff, value, mask);
    }
    return value;
}

__device__ __forceinline__ float block_reduce_sum(float value) {
    static __shared__ float shared[32];
    int lane = threadIdx.x & (kWarpSize - 1);
    int wid = threadIdx.x >> 5;
    value = warp_reduce_sum(value);
    if (lane == 0) {
        shared[wid] = value;
    }
    __syncthreads();
    value = (threadIdx.x < (blockDim.x + kWarpSize - 1) / kWarpSize) ? shared[lane] : 0.0f;
    if (wid == 0) {
        value = warp_reduce_sum(value);
    }
    if (threadIdx.x == 0) {
        shared[0] = value;
    }
    __syncthreads();
    return shared[0];
}

__global__ void embedding_prefill_kernel(const int32_t* input_ids,
                                         const half* embedding,
                                         half* output,
                                         int32_t hidden) {
    int32_t token_idx = blockIdx.x;
    int32_t hidden_idx = blockIdx.y * blockDim.x + threadIdx.x;
    if (hidden_idx >= hidden) {
        return;
    }
    int32_t token_id = input_ids[token_idx];
    const half* token_embedding = embedding + static_cast<int64_t>(token_id) * hidden;
    output[token_idx * hidden + hidden_idx] = token_embedding[hidden_idx];
}

__global__ void embedding_decode_kernel(const int32_t* input_ids,
                                        const half* embedding,
                                        half* output,
                                        int32_t hidden) {
    int32_t hidden_idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (hidden_idx >= hidden) {
        return;
    }
    int32_t token_id = input_ids[0];
    const half* token_embedding = embedding + static_cast<int64_t>(token_id) * hidden;
    output[hidden_idx] = token_embedding[hidden_idx];
}

__global__ void rmsnorm_kernel(const half* input,
                               const half* gamma,
                               half* output,
                               float epsilon,
                               int32_t hidden) {
    int32_t row = blockIdx.x;
    const half* row_input = input + static_cast<int64_t>(row) * hidden;
    half* row_output = output + static_cast<int64_t>(row) * hidden;
    float local_sum = 0.0f;
    for (int i = threadIdx.x; i < hidden; i += blockDim.x) {
        float val = __half2float(row_input[i]);
        local_sum += val * val;
    }
    float sum = block_reduce_sum(local_sum);
    __shared__ float inv_rms;
    if (threadIdx.x == 0) {
        inv_rms = rsqrtf(sum / static_cast<float>(hidden) + epsilon);
    }
    __syncthreads();

    for (int i = threadIdx.x; i < hidden; i += blockDim.x) {
        float x = __half2float(row_input[i]);
        float normed = x * inv_rms * __half2float(gamma[i]);
        row_output[i] = __float2half(normed);
    }
}

__global__ void qk_norm_rope_kernel(const half* input,
                                    const half* gamma,
                                    half* output,
                                    float epsilon,
                                    int32_t num_heads,
                                    int32_t head_dim,
                                    int32_t rotary_dim,
                                    int32_t tokens_per_batch,
                                    int32_t seq_pos,
                                    const float* cos_table,
                                    const float* sin_table) {
    int32_t token = blockIdx.x;
    int32_t head = blockIdx.y;
    int32_t offset = (token * num_heads + head) * head_dim;
    const half* head_input = input + offset;
    half* head_output = output + offset;

    float local_sum = 0.0f;
    for (int i = threadIdx.x; i < head_dim; i += blockDim.x) {
        float val = __half2float(head_input[i]);
        local_sum += val * val;
    }
    float sum = block_reduce_sum(local_sum);
    __shared__ float inv_rms;
    if (threadIdx.x == 0) {
        inv_rms = rsqrtf(sum / static_cast<float>(head_dim) + epsilon);
    }
    __syncthreads();

    int32_t position = tokens_per_batch > 1 ? (token % tokens_per_batch) : seq_pos;
    int32_t half_rotary = rotary_dim / 2;
    const float* cos_row = cos_table + position * half_rotary;
    const float* sin_row = sin_table + position * half_rotary;

    for (int i = threadIdx.x; i < half_rotary; i += blockDim.x) {
        int idx0 = i;
        int idx1 = i + half_rotary;
        float val0 = __half2float(head_input[idx0]);
        float val1 = __half2float(head_input[idx1]);
        float norm0 = val0 * inv_rms * __half2float(gamma[head * head_dim + idx0]);
        float norm1 = val1 * inv_rms * __half2float(gamma[head * head_dim + idx1]);
        float cos_val = cos_row[i];
        float sin_val = sin_row[i];
        float rot0 = norm0 * cos_val - norm1 * sin_val;
        float rot1 = norm0 * sin_val + norm1 * cos_val;
        head_output[idx0] = __float2half(rot0);
        head_output[idx1] = __float2half(rot1);
    }

    for (int i = rotary_dim + threadIdx.x; i < head_dim; i += blockDim.x) {
        float val = __half2float(head_input[i]);
        float normed = val * inv_rms * __half2float(gamma[head * head_dim + i]);
        head_output[i] = __float2half(normed);
    }
}

__global__ void k_norm_rope_kvcache_kernel(const half* key_input,
                                           const half* value_input,
                                           const half* gamma,
                                           half* key_output,
                                           half* key_cache,
                                           half* value_cache,
                                           int32_t seq_pos,
                                           float epsilon,
                                           int32_t num_heads,
                                           int32_t head_dim,
                                           int32_t rotary_dim,
                                           int32_t stride_tokens,
                                           int32_t stride_heads,
                                           int32_t stride_batch,
                                           const float* cos_table,
                                           const float* sin_table) {
    int32_t batch = blockIdx.x / num_heads;
    int32_t head = blockIdx.x % num_heads;
    int32_t offset = (batch * num_heads + head) * head_dim;
    const half* head_input = key_input + offset;
    const half* head_value = value_input + offset;
    half* head_output = key_output + offset;

    float local_sum = 0.0f;
    for (int i = threadIdx.x; i < head_dim; i += blockDim.x) {
        float val = __half2float(head_input[i]);
        local_sum += val * val;
    }
    float sum = block_reduce_sum(local_sum);
    __shared__ float inv_rms;
    if (threadIdx.x == 0) {
        inv_rms = rsqrtf(sum / static_cast<float>(head_dim) + epsilon);
    }
    __syncthreads();

    int32_t half_rotary = rotary_dim / 2;
    const float* cos_row = cos_table + seq_pos * half_rotary;
    const float* sin_row = sin_table + seq_pos * half_rotary;

    for (int i = threadIdx.x; i < half_rotary; i += blockDim.x) {
        int idx0 = i;
        int idx1 = i + half_rotary;
        float val0 = __half2float(head_input[idx0]);
        float val1 = __half2float(head_input[idx1]);
        float norm0 = val0 * inv_rms * __half2float(gamma[head * head_dim + idx0]);
        float norm1 = val1 * inv_rms * __half2float(gamma[head * head_dim + idx1]);
        float cos_val = cos_row[i];
        float sin_val = sin_row[i];
        float rot0 = norm0 * cos_val - norm1 * sin_val;
        float rot1 = norm0 * sin_val + norm1 * cos_val;
        head_output[idx0] = __float2half(rot0);
        head_output[idx1] = __float2half(rot1);
    }
    for (int i = rotary_dim + threadIdx.x; i < head_dim; i += blockDim.x) {
        float val = __half2float(head_input[i]);
        float normed = val * inv_rms * __half2float(gamma[head * head_dim + i]);
        head_output[i] = __float2half(normed);
    }
    __syncthreads();

    int64_t cache_offset = static_cast<int64_t>(batch) * stride_batch +
                           static_cast<int64_t>(head) * stride_heads +
                           static_cast<int64_t>(seq_pos) * stride_tokens;
    half* key_cache_ptr = key_cache + cache_offset;
    half* value_cache_ptr = value_cache + cache_offset;

    bool aligned = ((reinterpret_cast<uintptr_t>(head_output) & 0x3) == 0) &&
                   ((reinterpret_cast<uintptr_t>(head_value) & 0x3) == 0) &&
                   ((reinterpret_cast<uintptr_t>(key_cache_ptr) & 0x3) == 0) &&
                   ((reinterpret_cast<uintptr_t>(value_cache_ptr) & 0x3) == 0) &&
                   ((stride_tokens % 2) == 0) &&
                   ((stride_heads % 2) == 0) &&
                   ((stride_batch % 2) == 0);
    if (aligned && (head_dim % 2 == 0)) {
        int32_t vec_elems = head_dim / 2;
        const half2* key_out_vec = reinterpret_cast<const half2*>(head_output);
        const half2* val_vec = reinterpret_cast<const half2*>(head_value);
        half2* key_cache_vec = reinterpret_cast<half2*>(key_cache_ptr);
        half2* value_cache_vec = reinterpret_cast<half2*>(value_cache_ptr);
        for (int i = threadIdx.x; i < vec_elems; i += blockDim.x) {
            key_cache_vec[i] = key_out_vec[i];
            value_cache_vec[i] = val_vec[i];
        }
    } else {
        for (int i = threadIdx.x; i < head_dim; i += blockDim.x) {
            key_cache_ptr[i] = head_output[i];
            value_cache_ptr[i] = head_value[i];
        }
    }
}

__global__ void swiglu_kernel(const half* input, half* output, int32_t rows, int32_t cols) {
    int32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    int32_t total = rows * cols;
    if (idx >= total) {
        return;
    }
    int32_t col = idx % cols;
    int32_t row = idx / cols;
    int32_t offset = row * cols * 2;
    float gate = __half2float(input[offset + col]);
    float up = __half2float(input[offset + cols + col]);
    float silu = gate / (1.0f + expf(-gate));
    output[row * cols + col] = __float2half(silu * up);
}

__global__ void pack_gate_up_kernel(const half* gate,
                                    const half* up,
                                    half* packed,
                                    int32_t hidden,
                                    int32_t intermediate) {
    int32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    int32_t total = hidden * intermediate;
    if (idx >= total) {
        return;
    }
    int32_t h = idx / intermediate;
    int32_t i = idx % intermediate;
    int32_t packed_offset = h * intermediate * 2 + i;
    packed[packed_offset] = gate[h * intermediate + i];
    packed[packed_offset + intermediate] = up[h * intermediate + i];
}

} // namespace

cudaError_t embedding_forward(const EmbeddingParams& params, Stage stage, cudaStream_t stream) {
    if (stage == Stage::Prefill) {
        dim3 block(128);
        dim3 grid(params.batch * params.tokens, (params.hidden + block.x - 1) / block.x);
        embedding_prefill_kernel<<<grid, block, 0, stream>>>(
            params.input_ids,
            params.embedding_table,
            params.output,
            params.hidden);
    } else {
        dim3 block(128);
        dim3 grid((params.hidden + block.x - 1) / block.x);
        embedding_decode_kernel<<<grid, block, 0, stream>>>(
            params.input_ids,
            params.embedding_table,
            params.output,
            params.hidden);
    }
    return cudaGetLastError();
}

cublasStatus_t gemm_row_major(cublasHandle_t cublas,
                              const half* a,
                              const half* b,
                              half* c,
                              int32_t m,
                              int32_t n,
                              int32_t k) {
    const half alpha = __float2half(1.0f);
    const half beta = __float2half(0.0f);
    // Row-major C = A * B (A=[m,k], B=[k,n]) mapped to column-major:
    // C_col(n x m) = B_col(n x k) * A_col(k x m).
    return cublasGemmEx(cublas,
                        CUBLAS_OP_N,
                        CUBLAS_OP_N,
                        n,
                        m,
                        k,
                        &alpha,
                        b,
                        CUDA_R_16F,
                        n,
                        a,
                        CUDA_R_16F,
                        k,
                        &beta,
                        c,
                        CUDA_R_16F,
                        n,
                        CUDA_R_32F,
                        CUBLAS_GEMM_DEFAULT_TENSOR_OP);
}

cudaError_t rms_qkv_forward(const RmsQkvParams& params,
                            Stage stage,
                            cudaStream_t stream,
                            cublasHandle_t cublas) {
    int32_t rows = params.batch * params.tokens;
    int32_t out_hidden = params.hidden * 3;
    dim3 block(256);
    dim3 grid(rows);
    rmsnorm_kernel<<<grid, block, 0, stream>>>(
        params.input,
        params.rms_gamma,
        params.rms_out,
        params.epsilon,
        params.hidden);

    cublasSetStream(cublas, stream);
    cublasStatus_t qkv_status = gemm_row_major(cublas,
                                               params.rms_out,
                                               params.weight,
                                               params.qkv,
                                               rows,
                                               out_hidden,
                                               params.hidden);
    if (qkv_status != CUBLAS_STATUS_SUCCESS) {
        return cudaErrorUnknown;
    }
    (void)stage;
    return cudaGetLastError();
}

cudaError_t qk_norm_rope_forward(const QkNormRopeParams& params, Stage stage, cudaStream_t stream) {
    if ((params.rope.head_dim % 2) != 0 || (params.rope.rotary_dim % 2) != 0 ||
        params.rope.rotary_dim > params.rope.head_dim) {
        return cudaErrorInvalidValue;
    }
    int32_t rows = params.batch * params.tokens;
    dim3 block(128);
    dim3 grid(rows, params.num_heads);
    qk_norm_rope_kernel<<<grid, block, 0, stream>>>(
        params.input,
        params.norm_gamma,
        params.output,
        params.epsilon,
        params.num_heads,
        params.rope.head_dim,
        params.rope.rotary_dim,
        params.tokens,
        params.seq_pos,
        params.rope.cos_table,
        params.rope.sin_table);
    (void)stage;
    return cudaGetLastError();
}

cudaError_t k_norm_rope_kvcache_decode(const KNormRopeCacheParams& params,
                                       const KvCacheParams& cache,
                                       cudaStream_t stream) {
    if ((params.head_dim % 2) != 0 || (params.rope.rotary_dim % 2) != 0 ||
        params.rope.rotary_dim > params.head_dim) {
        return cudaErrorInvalidValue;
    }
    dim3 block(128);
    dim3 grid(params.batch * params.num_heads);
    k_norm_rope_kvcache_kernel<<<grid, block, 0, stream>>>(
        params.key_input,
        params.value_input,
        params.norm_gamma,
        params.key_output,
        cache.key_cache,
        cache.value_cache,
        params.seq_pos,
        params.epsilon,
        params.num_heads,
        params.head_dim,
        params.rope.rotary_dim,
        cache.stride_tokens,
        cache.stride_heads,
        cache.stride_batch,
        params.rope.cos_table,
        params.rope.sin_table);
    return cudaGetLastError();
}

cudaError_t attention_forward_flash_v2(const AttentionParams& params,
                                       Stage stage,
                                       cudaStream_t stream) {
#ifdef QWEN3_USE_FLASHATTN_V2
    extern cudaError_t flashattn_v2_forward(const AttentionParams&, Stage, cudaStream_t);
    return flashattn_v2_forward(params, stage, stream);
#else
    (void)params;
    (void)stage;
    (void)stream;
    return cudaErrorNotSupported;
#endif
}

cudaError_t ffn_pack_w1(const half* w_gate,
                        const half* w_up,
                        half* packed_w1,
                        int32_t hidden,
                        int32_t intermediate,
                        cudaStream_t stream) {
    int threads = 256;
    int blocks = (hidden * intermediate + threads - 1) / threads;
    pack_gate_up_kernel<<<blocks, threads, 0, stream>>>(
        w_gate,
        w_up,
        packed_w1,
        hidden,
        intermediate);
    return cudaGetLastError();
}

cudaError_t ffn_swiglu_forward(const FfnParams& params,
                              Stage stage,
                              cudaStream_t stream,
                              cublasHandle_t cublas) {
    int32_t rows = params.batch * params.tokens;
    int32_t cols = params.intermediate;
    if (params.workspace == nullptr || params.workspace_bytes < sizeof(half) * rows * cols * 3) {
        return cudaErrorInvalidValue;
    }
    if (params.packed_w1 == nullptr) {
        return cudaErrorInvalidValue;
    }
    auto* workspace_ptr = reinterpret_cast<half*>(params.workspace);
    half* gate_up = workspace_ptr;
    half* hidden = gate_up + rows * cols * 2;

    cublasSetStream(cublas, stream);
    cublasStatus_t gate_status = gemm_row_major(cublas,
                                                params.input,
                                                params.packed_w1,
                                                gate_up,
                                                rows,
                                                cols * 2,
                                                params.hidden);
    if (gate_status != CUBLAS_STATUS_SUCCESS) {
        return cudaErrorUnknown;
    }

    int threads = 256;
    int blocks = (rows * cols + threads - 1) / threads;
    swiglu_kernel<<<blocks, threads, 0, stream>>>(gate_up, hidden, rows, cols);

    cublasStatus_t down_status = gemm_row_major(cublas,
                                                hidden,
                                                params.w_down,
                                                params.output,
                                                rows,
                                                params.hidden,
                                                cols);
    if (down_status != CUBLAS_STATUS_SUCCESS) {
        return cudaErrorUnknown;
    }
    (void)stage;
    return cudaGetLastError();
}

} // namespace qwen3
