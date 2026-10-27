// cudabench: device-memory copy kernel on one GPU for N seconds; reports rd+wr GB/s.
#include <cstdio>
#include <cstdlib>
#include <chrono>
__global__ void cp(const float4 *a, float4 *b, size_t n) {
    for (size_t i = blockIdx.x * blockDim.x + threadIdx.x; i < n; i += (size_t)gridDim.x * blockDim.x) b[i] = a[i];
}
int main(int argc, char **argv) {
    int dev = argc > 1 ? atoi(argv[1]) : 0;
    double secs = argc > 2 ? atof(argv[2]) : 5;
    double frac = argc > 3 ? atof(argv[3]) : 1;   // duty cycle 0..1 to produce partial load
    cudaSetDevice(dev);
    size_t n = (1ul << 30) / sizeof(float4);
    float4 *a, *b;
    cudaMalloc(&a, n * sizeof(float4)); cudaMalloc(&b, n * sizeof(float4));
    cudaMemset(a, 0, n * sizeof(float4));
    auto t0 = std::chrono::steady_clock::now();
    double bytes = 0, el = 0;
    while (el < secs) {
        auto k0 = std::chrono::steady_clock::now();
        cp<<<84 * 16, 256>>>(a, b, n); cudaDeviceSynchronize();
        bytes += 2.0 * n * sizeof(float4);
        double kt = std::chrono::duration<double>(std::chrono::steady_clock::now() - k0).count();
        if (frac < 1) { auto s0 = std::chrono::steady_clock::now(); while (std::chrono::duration<double>(std::chrono::steady_clock::now() - s0).count() < kt * (1 / frac - 1)); }
        el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
    printf("cudabench gpu%d: %.1f GB/s (rd+wr, duty %.2f)\n", dev, bytes / el / 1e9, frac);
}
