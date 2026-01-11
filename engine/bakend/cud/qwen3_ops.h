#pragma once

#include <cstddef>
#include <cstdint>

#include <cuda_fp16.h>
#include <cuda_runtime.h>
#include <cublas_v2.h>

namespace qwen3 {

enum class Stage {
    Prefill,
    Decode
};

struct EmbeddingParams {
    const int32_t* input_ids;   // [B, T]
    const half* embedding_table; // [V, H]
    half* output;               // [B, T, H]
    int32_t batch;
    int32_t tokens;
    int32_t hidden;
};

struct RmsQkvParams {
    const half* input;    // [B, T, H]
    const half* weight;   // [H, 3*H]
    const half* rms_gamma; // [H]
    half* qkv;            // [B, T, 3*H]
    half* rms_out;        // [B, T, H] (workspace)
    float epsilon;
    int32_t batch;
    int32_t tokens;
    int32_t hidden;
};

struct RopeParams {
    float rope_theta;
    int32_t head_dim;
    int32_t rotary_dim;
    int32_t max_seq_len;
    const float* cos_table; // [max_seq_len, rotary_dim / 2]
    const float* sin_table; // [max_seq_len, rotary_dim / 2]
};

struct QkNormRopeParams {
    const half* input;      // [B, T, H]
    const half* norm_gamma; // [H]
    half* output;           // [B, T, H]
    float epsilon;
    int32_t batch;
    int32_t tokens;
    int32_t hidden;
    int32_t num_heads;
    int32_t seq_pos;        // decode position when tokens == 1
    RopeParams rope;
};

struct KNormRopeCacheParams {
    const half* key_input;    // [B, H]
    const half* value_input;  // [B, H]
    const half* norm_gamma;   // [H]
    half* key_output;         // [B, H]
    float epsilon;
    int32_t batch;
    int32_t num_heads;
    int32_t head_dim;
    int32_t seq_pos;
    RopeParams rope;
};

struct KvCacheParams {
    half* key_cache;     // [B, num_heads, max_seq, head_dim]
    half* value_cache;   // [B, num_heads, max_seq, head_dim]
    int32_t max_seq_len;
    int32_t num_heads;
    int32_t head_dim;
    int32_t stride_tokens; // elements between tokens
    int32_t stride_heads;  // elements between heads
    int32_t stride_batch;  // elements between batches
};

struct AttentionParams {
    const half* query; // [B, T, H]
    const half* key;   // [B, T, H] or cached key for decode
    const half* value; // [B, T, H] or cached value for decode
    const half* attn_mask; // optional, [B, T, T]
    half* output;      // [B, T, H]
    int32_t batch;
    int32_t tokens;
    int32_t hidden;
    int32_t num_heads;
    int32_t head_dim;
};

struct FfnParams {
    const half* input;    // [B, T, H]
    const half* w_gate;   // [H, I] (optional if packed_w1 provided)
    const half* w_up;     // [H, I] (optional if packed_w1 provided)
    const half* w_down;   // [I, H]
    const half* packed_w1; // [H, 2*I], packed gate+up
    half* output;         // [B, T, H]
    void* workspace;      // workspace buffer
    size_t workspace_bytes;
    int32_t batch;
    int32_t tokens;
    int32_t hidden;
    int32_t intermediate;
};

cudaError_t embedding_forward(const EmbeddingParams& params, Stage stage, cudaStream_t stream);

cudaError_t rms_qkv_forward(const RmsQkvParams& params,
                            Stage stage,
                            cudaStream_t stream,
                            cublasHandle_t cublas);

cudaError_t qk_norm_rope_forward(const QkNormRopeParams& params, Stage stage, cudaStream_t stream);

cudaError_t k_norm_rope_kvcache_decode(const KNormRopeCacheParams& params,
                                       const KvCacheParams& cache,
                                       cudaStream_t stream);

cudaError_t attention_forward_flash_v2(const AttentionParams& params,
                                       Stage stage,
                                       cudaStream_t stream);

cudaError_t ffn_pack_w1(const half* w_gate,
                        const half* w_up,
                        half* packed_w1,
                        int32_t hidden,
                        int32_t intermediate,
                        cudaStream_t stream);

cudaError_t ffn_swiglu_forward(const FfnParams& params,
                              Stage stage,
                              cudaStream_t stream,
                              cublasHandle_t cublas);

} // namespace qwen3
