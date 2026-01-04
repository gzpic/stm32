// __global__ void swish(const half* input, half* output, int size)
// {
//     int idx = blockIdx.x * blockDim.x + threadIdx.x;
//     float x = __half2float(input[idx]); // 转换函数
//     float y;

//     if (idx < size) {
//         y = __float2half(x /(1 + expf(-x))); // expf的使用
//         output[idx] = y;
//     }
// }




// __device__ half swish(const half input)
// {
//     float x = __half2float(input); // 转换函数
//     float y = x /(1.0f + expf(-x)); // expf的使用，要用1.0f
//     return __float2half(y);
// }

// // 向量化加载
// __global__ void h_swish(const half* input, half* output, int size)
// {
//     int tdx = blockIdx.x * blockDim.x + threadIdx.x;
//     int idx = tdx * 2;
//     float y;

//     if (idx + 1 < size) {

//         half2 xv = reinterpret_cast<half2*>(input)[tdx];// 为啥这tdx，reinterpret_cast<half2*>(input)[tid] 中必须用 tid，
//                                                         //因为 half2* 的索引单位已经是 “2 个 half” 了。
//         half x0 = xv.x;
//         half x1 = xv.y;

//         output[idx] = swish(x0);
//         output[idx + 1] = swish(x1);
//     }
// }


// __global__ void elementwise(const half* A, const half* B, half *C, int size)
// {




//     int tid = blockIdx.x * blockDim.x + threadIdx.x;
//     int idx = tid * 2;

//     if (idx + 1 < size) {
//         const half2 av = reinterpret_cast<half2 *>(A)[tid]; // 指令的用法
//         const half2 bv = reinterpret_cast<half2 *>(B)[tid];

//         half2 cv = __hmul2(av, bv);
//         reinterpret_cast<half2*>(C)[tid] = cv;

//     }
// }



// todo
/ GEMM kernel配置参数
#define BLOCK_SIZE_M 128  // Block tile M维度
#define BLOCK_SIZE_N 128  // Block tile N维度
#define BLOCK_SIZE_K 16   // Block tile K维度
#define THREAD_SIZE_M 8   // 每个线程处理的M维度元素数
#define THREAD_SIZE_N 8   // 每个线程处理的N维度元素数

__global__ void gemm(const half* A, const half* B, half* C, const int M, const int N, const int K)
{
    // Block索引
    const int bx = blockIdx.x;
    const int by = blockIdx.y;
    
    // Thread索引
    const int tx = threadIdx.x;
    const int ty = threadIdx.y;
    const int tid = ty * blockDim.x + tx;
    
    // 计算线程块在输出矩阵中的起始位置
    const int block_row = by * BLOCK_SIZE_M;
    const int block_col = bx * BLOCK_SIZE_N;
    
    // 双缓存共享内存：用于A和B矩阵的tile
    // 使用两个buffer进行乒乓交换
    __shared__ half As[2][BLOCK_SIZE_M][BLOCK_SIZE_K];
    __shared__ half Bs[2][BLOCK_SIZE_K][BLOCK_SIZE_N];
    
    // 每个线程负责的寄存器累加器
    float accum[THREAD_SIZE_M][THREAD_SIZE_N];
    
    // 初始化累加器
    #pragma unroll
    for (int i = 0; i < THREAD_SIZE_M; ++i) {
        #pragma unroll
        for (int j = 0; j < THREAD_SIZE_N; ++j) {
            accum[i][j] = 0.0f;
        }
    }
    
    // 计算每个线程在shared memory中负责加载的位置
    const int num_threads = blockDim.x * blockDim.y;
    const int load_a_smem_m = (tid / (BLOCK_SIZE_K / 8)) * 8;  // 每个线程加载8个元素
    const int load_a_smem_k = (tid % (BLOCK_SIZE_K / 8)) * 8;
    const int load_b_smem_k = (tid / (BLOCK_SIZE_N / 8)) * 8;
    const int load_b_smem_n = (tid % (BLOCK_SIZE_N / 8)) * 8;
    
    // 全局内存中的加载位置
    const int load_a_gmem_m = block_row + load_a_smem_m;
    const int load_b_gmem_n = block_col + load_b_smem_n;
    
    // 计算K维度需要迭代的次数
    const int num_k_tiles = (K + BLOCK_SIZE_K - 1) / BLOCK_SIZE_K;
    
    // write_stage_idx表示当前写入哪个buffer (0或1)
    int write_stage_idx = 0;
    
    // ==== 预加载第一个tile到buffer 0 ====
    {
        int load_a_gmem_k = load_a_smem_k;
        int load_a_gmem_addr = load_a_gmem_m * K + load_a_gmem_k;
        int load_b_gmem_k = load_b_smem_k;
        int load_b_gmem_addr = load_b_gmem_k * N + load_b_gmem_n;
        
        // 加载A的tile (每个线程加载8个half元素 = 128bit)
        if (load_a_gmem_m < M && load_a_gmem_k < K) {
            int4 a_data = *reinterpret_cast<const int4*>(&A[load_a_gmem_addr]);
            *reinterpret_cast<int4*>(&As[0][load_a_smem_m][load_a_smem_k]) = a_data;
        } else {
            #pragma unroll
            for (int i = 0; i < 8; ++i) {
                As[0][load_a_smem_m][load_a_smem_k + i] = __float2half(0.0f);
            }
        }
        
        // 加载B的tile
        if (load_b_gmem_k < K && load_b_gmem_n < N) {
            int4 b_data = *reinterpret_cast<const int4*>(&B[load_b_gmem_addr]);
            *reinterpret_cast<int4*>(&Bs[0][load_b_smem_k][load_b_smem_n]) = b_data;
        } else {
            #pragma unroll
            for (int i = 0; i < 8; ++i) {
                Bs[0][load_b_smem_k][load_b_smem_n + i] = __float2half(0.0f);
            }
        }
        
        __syncthreads();
        write_stage_idx ^= 1;  // 切换到buffer 1
    }
    
    // ==== 主循环：使用双缓存流水线 ====
    #pragma unroll 1
    for (int k_tile = 0; k_tile < num_k_tiles; ++k_tile) {
        // 当前计算使用的buffer index (read)
        int compute_stage_idx = write_stage_idx ^ 1;
        
        // 如果不是最后一个tile，异步加载下一个tile到write_stage_idx
        if (k_tile + 1 < num_k_tiles) {
            int next_k_tile = k_tile + 1;
            int load_a_gmem_k = next_k_tile * BLOCK_SIZE_K + load_a_smem_k;
            int load_a_gmem_addr = load_a_gmem_m * K + load_a_gmem_k;
            int load_b_gmem_k = next_k_tile * BLOCK_SIZE_K + load_b_smem_k;
            int load_b_gmem_addr = load_b_gmem_k * N + load_b_gmem_n;
            
            // 加载下一个A tile到write_stage_idx
            if (load_a_gmem_m < M && load_a_gmem_k < K) {
                int4 a_data = *reinterpret_cast<const int4*>(&A[load_a_gmem_addr]);
                *reinterpret_cast<int4*>(&As[write_stage_idx][load_a_smem_m][load_a_smem_k]) = a_data;
            } else {
                #pragma unroll
                for (int i = 0; i < 8; ++i) {
                    As[write_stage_idx][load_a_smem_m][load_a_smem_k + i] = __float2half(0.0f);
                }
            }
            
            // 加载下一个B tile到write_stage_idx
            if (load_b_gmem_k < K && load_b_gmem_n < N) {
                int4 b_data = *reinterpret_cast<const int4*>(&B[load_b_gmem_addr]);
                *reinterpret_cast<int4*>(&Bs[write_stage_idx][load_b_smem_k][load_b_smem_n]) = b_data;
            } else {
                #pragma unroll
                for (int i = 0; i < 8; ++i) {
                    Bs[write_stage_idx][load_b_smem_k][load_b_smem_n + i] = __float2half(0.0f);
                }
            }
        }
        
        // ==== 计算当前tile (使用compute_stage_idx的数据) ====
        // 每个线程计算一个THREAD_SIZE_M x THREAD_SIZE_N的小块
        const int thread_m = ty * THREAD_SIZE_M;
        const int thread_n = tx * THREAD_SIZE_N;
        
        // 寄存器缓存
        half a_frag[THREAD_SIZE_M];
        half b_frag[THREAD_SIZE_N];
        
        #pragma unroll
        for (int k = 0; k < BLOCK_SIZE_K; ++k) {
            // 从shared memory加载到寄存器
            #pragma unroll
            for (int i = 0; i < THREAD_SIZE_M; ++i) {
                a_frag[i] = As[compute_stage_idx][thread_m + i][k];
            }
            
            #pragma unroll
            for (int j = 0; j < THREAD_SIZE_N; ++j) {
                b_frag[j] = Bs[compute_stage_idx][k][thread_n + j];
            }
            
            // 执行外积累加
            #pragma unroll
            for (int i = 0; i < THREAD_SIZE_M; ++i) {
                #pragma unroll
                for (int j = 0; j < THREAD_SIZE_N; ++j) {
                    accum[i][j] += __half2float(a_frag[i]) * __half2float(b_frag[j]);
                }
            }
        }
        
        // 同步，确保所有线程完成计算，然后可以安全地加载下一个tile
        __syncthreads();
        
        // 切换buffer
        write_stage_idx ^= 1;
    }
    
    // ==== 将结果写回全局内存 ====
    const int thread_m = ty * THREAD_SIZE_M;
    const int thread_n = tx * THREAD_SIZE_N;
    
    #pragma unroll
    for (int i = 0; i < THREAD_SIZE_M; ++i) {
        #pragma unroll
        for (int j = 0; j < THREAD_SIZE_N; ++j) {
            int c_row = block_row + thread_m + i;
            int c_col = block_col + thread_n + j;
            
            if (c_row < M && c_col < N) {
                C[c_row * N + c_col] = __float2half(accum[i][j]);
            }
        }
    }
}





__device__ half swish(const half input) // 函数可优化 todo
{
    float x = __half2float(input); // 转换函数
    float y = x /(1.0f + expf(-x)); // expf的使用，要用1.0f
    return __float2half(y);
}

// 要不要使用一个block处理一个token，这先不这样
//  X:[B*S, H] @ W[gate,up]:[H, 2I] = [B*S,2I],左一半是gate，右一半up

// 实际的shape，[B,S,H],FFN常按照[B*S,H]来处理，写kernel的时候入参[B*S*H],内部要视为[B*S,H]，出参还[B*S*H]，
__global__ void fnn_swiglu(const half* input, half* output, const int M, const int I // ffn的隐藏维维度,M = b * S.
)
{
    int tid = blockIdx.x * blockDim.x + threadIdx.x;   // 每个线程处理一行[1, 2I],前一半gate，后一半up
    // todo，input里既有Hgate又有Hup,具体用交错布局还是分块布局优缺点？交错效率更高，先按分区放，在来交错放
    int tokenId = tid / I;
    int eleId = tid % I;

    if (tid >= M*I) return;

    half Hgate = input[tokenId * 2 * I + eleId];
    half Hup = input[tokenId * 2 * I + eleId + I];

    // 先计算swish(Hgate)
    Hgate = swish(Hgate);
    output[tokenId * I + eleId] = __hmul(Hgate, Hup);
}

