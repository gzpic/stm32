#include <cuda_runtime.h>



#define FINAL_MASK 0xffffffff
// todo
template <typename T>
__inline__ __device__
T warpReduceSum(T val)
{
    for(int mask = 16; mask > 0; mask >>= 1) {
        val += __shfl_xor_sync(FINAL_MASK, val, mask, 32);
    }
    return val;
}
// todo
template <typename T>
__inline__ __device__
T blockReduceSum(T val)
{
    static __shared__ T shared[32];
    int lane = threadIdx.x & 0x1f;
    int wid = threadIdx.x >> 5;

    val = warpReduceSum<T>(val);

    if(lane == 0) {
        shared[wid] = val;
    }
    __syncthreads();

    val = (threadIdx.x < (blockDim.x >> 5 )) ? shared[lane] : (T)0.0f;
    val = warpReduceSum(val);
    return val;
}




#include <cuda_runtime.h>
#include <cuda_fp16.h>

#define FINAL_MASK 0xffffffff

template <typename T>
__inline__ __device__ T warpReduceSum(T val) {
    for (int mask = 16; mask > 0; mask >>= 1) {
        val += __shfl_xor_sync(FINAL_MASK, val, mask, 32);
    }
    return val;
}

template <typename T>
__inline__ __device__ T blockReduceSum(T val) {
    static __shared__ T shared[32]; 
    int lane = threadIdx.x & 0x1f;
    int wid = threadIdx.x >> 5;

    val = warpReduceSum<T>(val);

    if (lane == 0) shared[wid] = val;
    __syncthreads();

    // 读取各 warp 的结果，如果 blockDim.x < 1024，未使用的部分补 0
    val = (threadIdx.x < (blockDim.x + 31) / 32) ? shared[lane] : (T)0.0f;
    
    if (wid == 0) val = warpReduceSum<T>(val);

    return val; 
}

// 假设每个 Block 处理向量的一行，n 为向量长度
__global__ void RmsNorm_fp16_Corrected(const half* A, half* B, int n, float epsilon) {
    float thread_sum = 0.0f;
    
    // 1. 每个线程累加自己负责的元素的平方
    for (int i = threadIdx.x; i < n; i += blockDim.x) {
        float val = __half2float(A[i]);
        thread_sum += val * val;
    }

    // 2. Block 内规约得到总平方和
    float total_sum = blockReduceSum<float>(thread_sum);

    // 3. 计算 RMS 因子 (所有线程共享这个结果)
    __shared__ float shared_rms;
    if (threadIdx.x == 0) {
        shared_rms = rsqrtf(total_sum / n + epsilon);
    }
    __syncthreads();

    // 4. 写回结果
    for (int i = threadIdx.x; i < n; i += blockDim.x) {
        B[i] = __float2half(__half2float(A[i]) * shared_rms);
    }
}