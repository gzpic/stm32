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
    return value;
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

__global__ void rms_qkv_prefill_kernel(const half* input,
                                       const half* weight,
                                       const half* gamma,
                                       half* qkv,
                                       float epsilon,
                                       int32_t hidden,
                                       int32_t out_hidden) {
    int32_t row = blockIdx.x;
    const half* row_input = input + static_cast<int64_t>(row) * hidden;
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

    for (int out_col = blockIdx.y * blockDim.x + threadIdx.x; out_col < out_hidden; out_col += gridDim.y * blockDim.x) {
        float acc = 0.0f;
        for (int k = 0; k < hidden; ++k) {
            float x = __half2float(row_input[k]);
            float normed = x * inv_rms * __half2float(gamma[k]);
            float w = __half2float(weight[k * out_hidden + out_col]);
            acc += normed * w;
        }
        qkv[row * out_hidden + out_col] = __float2half(acc);
    }
}

__device__ __forceinline__ void apply_rope(float& x0, float& x1, float theta, int position, int idx, int head_dim) {
    float freq = powf(theta, -static_cast<float>(2 * idx) / static_cast<float>(head_dim));
    float angle = static_cast<float>(position) * freq;
    float sin_val;
    float cos_val;
    sincosf(angle, &sin_val, &cos_val);
    float rot0 = x0 * cos_val - x1 * sin_val;
    float rot1 = x0 * sin_val + x1 * cos_val;
    x0 = rot0;
    x1 = rot1;
}

__global__ void qk_norm_rope_kernel(const half* input,
                                    const half* gamma,
                                    half* output,
                                    float epsilon,
                                    int32_t num_heads,
                                    int32_t head_dim,
                                    int32_t tokens_per_batch,
                                    int32_t seq_pos,
                                    float rope_theta) {
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

    for (int i = threadIdx.x; i < head_dim; i += blockDim.x) {
        float val = __half2float(head_input[i]);
        float normed = val * inv_rms * __half2float(gamma[head * head_dim + i]);
        head_output[i] = __float2half(normed);
    }
    __syncthreads();

    int32_t position = tokens_per_batch > 1 ? (token % tokens_per_batch) : seq_pos;
    for (int i = threadIdx.x; i < head_dim; i += blockDim.x * 2) {
        int idx = i;
        int pair = idx + head_dim / 2;
        if (pair < head_dim) {
            float x0 = __half2float(head_output[idx]);
            float x1 = __half2float(head_output[pair]);
            apply_rope(x0, x1, rope_theta, position, idx, head_dim);
            head_output[idx] = __float2half(x0);
            head_output[pair] = __float2half(x1);
        }
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
                                           int32_t max_seq,
                                           float rope_theta) {
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

    for (int i = threadIdx.x; i < head_dim; i += blockDim.x) {
        float val = __half2float(head_input[i]);
        float normed = val * inv_rms * __half2float(gamma[head * head_dim + i]);
        head_output[i] = __float2half(normed);
    }
    __syncthreads();

    for (int i = threadIdx.x; i < head_dim; i += blockDim.x * 2) {
        int idx = i;
        int pair = idx + head_dim / 2;
        if (pair < head_dim) {
            float x0 = __half2float(head_output[idx]);
            float x1 = __half2float(head_output[pair]);
            apply_rope(x0, x1, rope_theta, seq_pos, idx, head_dim);
            head_output[idx] = __float2half(x0);
            head_output[pair] = __float2half(x1);
        }
    }
    __syncthreads();

    int64_t cache_offset = (static_cast<int64_t>(batch) * num_heads + head) * max_seq * head_dim + static_cast<int64_t>(seq_pos) * head_dim;
    half* key_cache_ptr = key_cache + cache_offset;
    half* value_cache_ptr = value_cache + cache_offset;
    for (int i = threadIdx.x; i < head_dim; i += blockDim.x) {
        key_cache_ptr[i] = head_output[i];
        value_cache_ptr[i] = head_value[i];
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

cudaError_t rms_qkv_forward(const RmsQkvParams& params, Stage stage, cudaStream_t stream) {
    int32_t rows = params.batch * params.tokens;
    int32_t out_hidden = params.hidden * 3;
    dim3 block(128);
    dim3 grid(rows, 4);
    rms_qkv_prefill_kernel<<<grid, block, 0, stream>>>(
        params.input,
        params.weight,
        params.rms_gamma,
        params.qkv,
        params.epsilon,
        params.hidden,
        out_hidden);
    (void)stage;
    return cudaGetLastError();
}

cudaError_t qk_norm_rope_forward(const QkNormRopeParams& params, Stage stage, cudaStream_t stream) {
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
        params.tokens,
        params.seq_pos,
        params.rope.rope_theta);
    (void)stage;
    return cudaGetLastError();
}

cudaError_t k_norm_rope_kvcache_decode(const KNormRopeCacheParams& params,
                                       const KvCacheParams& cache,
                                       cudaStream_t stream) {
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
        cache.max_seq_len,
        params.rope.rope_theta);
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

cudaError_t ffn_swiglu_forward(const FfnParams& params, Stage stage, cudaStream_t stream, cublasHandle_t cublas) {
    int32_t rows = params.batch * params.tokens;
    int32_t cols = params.intermediate;
    half* gate_up = nullptr;
    cudaMalloc(&gate_up, sizeof(half) * rows * cols * 2);

    half* packed_weights = nullptr;
    cudaMalloc(&packed_weights, sizeof(half) * params.hidden * cols * 2);
    int pack_threads = 256;
    int pack_blocks = (params.hidden * cols + pack_threads - 1) / pack_threads;
    pack_gate_up_kernel<<<pack_blocks, pack_threads, 0, stream>>>(
        params.w_gate,
        params.w_up,
        packed_weights,
        params.hidden,
        cols);

    cublasSetStream(cublas, stream);
    const half alpha = __float2half(1.0f);
    const half beta = __float2half(0.0f);

    cublasGemmEx(cublas,
                CUBLAS_OP_N,
                CUBLAS_OP_N,
                cols * 2,
                rows,
                params.hidden,
                &alpha,
                packed_weights,
                CUDA_R_16F,
                cols * 2,
                params.input,
                CUDA_R_16F,
                params.hidden,
                &beta,
                gate_up,
                CUDA_R_16F,
                cols * 2,
                CUDA_R_32F,
                CUBLAS_GEMM_DEFAULT_TENSOR_OP);

    half* hidden = nullptr;
    cudaMalloc(&hidden, sizeof(half) * rows * cols);
    int threads = 256;
    int blocks = (rows * cols + threads - 1) / threads;
    swiglu_kernel<<<blocks, threads, 0, stream>>>(gate_up, hidden, rows, cols);

    cublasGemmEx(cublas,
                CUBLAS_OP_N,
                CUBLAS_OP_N,
                params.hidden,
                rows,
                cols,
                &alpha,
                params.w_down,
                CUDA_R_16F,
                params.hidden,
                hidden,
                CUDA_R_16F,
                cols,
                &beta,
                params.output,
                CUDA_R_16F,
                params.hidden,
                CUDA_R_32F,
                CUBLAS_GEMM_DEFAULT_TENSOR_OP);

    cudaFree(gate_up);
    cudaFree(hidden);
    cudaFree(packed_weights);
    (void)stage;
    return cudaGetLastError();
}

} // namespace qwen3
