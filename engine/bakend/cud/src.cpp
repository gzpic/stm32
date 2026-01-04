for (int h = 0; h < H; ++h) {
    for (int i = 0; i < I; ++i) {
        W_gated[h*2*I + i]      = W_gate[h*I + i]; // gate
        W_gated[h*2*I + I + i]  = W_up[h*I + i];   // up
    }
}



void ffn_forward_execute(
    const half* d_X,          // input [B*S,H], GPU pointer
    half* d_Y,                // output [B*S,H], GPU pointer
    const half* d_W_gate,     // [H,I]
    const half* d_W_up,       // [H,I]
    const half* d_W_down,     // [I,H]
    int B, int S, int H, int I
)
{
    int M = B * S;

    // --- 1️⃣ GEMM 上投影 ---
    // 输出缓冲区: [M,2I]
    half* d_GEMM_out;
    cudaMalloc(&d_GEMM_out, sizeof(half) * M * 2 * I);

    
    half* h_W_gate_up = new half[H * 2 * I];

    for (int h = 0; h < H; ++h) {
        for (int i = 0; i < I; ++i) {
            h_W_gate_up[h*2*I + i]      = W_gate[h*I + i]; // gate
            h_W_gate_up[h*2*I + I + i]  = W_up[h*I + i];   // up
        }
    }

    half* d_W_gate_up;
    cudaMalloc(&d_W_gate_up, sizeof(half) * H * 2 * I);
    cudaMemcpy(d_W_gate_up, h_W_gate_up, sizeof(half) * H * 2 * I, cudaMemcpyHostToDevice);

    delete[] h_W_gate_up;

    // 调用 cuBLAS / CUTLASS GEMM
    gemm(d_X, d_W_gate_up, d_GEMM_out, M, 2 * I, H);

    // --- 2️⃣ SwiGLU 激活 ---
    // 输出缓冲区: [M,I]
    half* d_FFN_hidden;
    cudaMalloc(&d_FFN_hidden, sizeof(half) * M * I);

    int threads = 256;
    int blocks  = (M * I + threads - 1) / threads;
    fnn_swiglu<<<blocks, threads, 0>>>(d_GEMM_out, d_FFN_hidden, M, I);

    // --- 3️⃣ GEMM 下投影 ---
    gemm(d_FFN_hidden, d_W_down, d_Y, M, I, H);

    // --- 4️⃣ 释放中间缓冲 ---
    cudaFree(d_GEMM_out);
    cudaFree(d_FFN_hidden);
    cudaFree(d_W_gate_up);
}