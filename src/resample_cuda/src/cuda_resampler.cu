#include "r3dss/resample_cuda/cuda_resampler.hpp"
#include <cuda_runtime.h>
#include <cmath>
#include <vector>
#include "r3dss/resample/taps.hpp"

namespace r3dss::resample {

int cudaDeviceCount() {
    int n = 0;
    if (cudaGetDeviceCount(&n) != cudaSuccess) return 0;
    return n;
}

std::string cudaDeviceName(int device) {
    cudaDeviceProp p{};
    if (cudaGetDeviceProperties(&p, device) != cudaSuccess) return "";
    return p.name;
}

uint64_t cudaDeviceMemory(int device) {
    cudaDeviceProp p{};
    if (cudaGetDeviceProperties(&p, device) != cudaSuccess) return 0;
    return p.totalGlobalMem;
}

namespace {

struct DevTaps {
    int32_t* start = nullptr;
    uint16_t* count = nullptr;
    float* weights = nullptr;
    uint32_t maxTaps = 0;
    uint32_t len = 0;
};

// Arithmetic mirrors cpu_resampler.cpp operation by operation: same tap order, every multiply and add rounded
// separately (__fmul_rn/__fadd_rn forbid the fused multiply-add nvcc would otherwise emit; the CPU library is
// built with -ffp-contract=off), and roundf (half away from zero, like std::round). CPU and GPU therefore take
// the same branch of the clipping rule at the white - 1 threshold.

// Horizontal pass: one thread per (output column, source plane row).
__global__ void kHorizontal(const uint16_t* __restrict__ src, uint32_t srcW, uint32_t ox, uint32_t oy, uint32_t sph,
                            const int32_t* __restrict__ start, const uint16_t* __restrict__ count,
                            const float* __restrict__ weights, uint32_t maxTaps, uint32_t dpw, float clipThr,
                            float* __restrict__ tmp, uint8_t* __restrict__ tmpClip) {
    const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    const uint32_t j = blockIdx.y * blockDim.y + threadIdx.y;
    if (i >= dpw || j >= sph) return;
    const uint16_t* row = src + (size_t)(2 * j + oy) * srcW + ox;
    const float* w = weights + (size_t)i * maxTaps;
    const int st = start[i];
    const int cnt = count[i];
    float acc = 0.0f;
    bool clip = false;
    for (int k = 0; k < cnt; ++k) {
        const float v = (float)row[(size_t)(st + k) * 2];
        acc = __fadd_rn(acc, __fmul_rn(w[k], v));
        clip |= v >= clipThr;
    }
    tmp[(size_t)j * dpw + i] = acc;
    if (tmpClip) tmpClip[(size_t)j * dpw + i] = clip ? 1 : 0;
}

// Vertical pass with the clipping rule (docs/RESAMPLING.md §4, contract I5), the Area fallback evaluated directly
// on the source plane. `tmpClip` is null when the rule is off (clip-aware disabled or Area kernel).
__global__ void kVertical(const float* __restrict__ tmp, const uint8_t* __restrict__ tmpClip, uint32_t dpw, uint32_t dph,
                          const int32_t* __restrict__ startY, const uint16_t* __restrict__ countY,
                          const float* __restrict__ weightsY, uint32_t maxTapsY,
                          const uint16_t* __restrict__ src, uint32_t srcW, uint32_t ox, uint32_t oy,
                          const int32_t* __restrict__ aStartX, const uint16_t* __restrict__ aCountX,
                          const float* __restrict__ aWX, uint32_t aMaxX,
                          const int32_t* __restrict__ aStartY, const uint16_t* __restrict__ aCountY,
                          const float* __restrict__ aWY, uint32_t aMaxY,
                          float clipThr, float ceiling, uint16_t* __restrict__ dst, uint32_t dstW) {
    const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    const uint32_t j = blockIdx.y * blockDim.y + threadIdx.y;
    if (i >= dpw || j >= dph) return;
    const float* w = weightsY + (size_t)j * maxTapsY;
    const int st = startY[j];
    const int cnt = countY[j];
    float acc = 0.0f;
    bool clip = false;
    for (int k = 0; k < cnt; ++k) {
        const size_t idx = (size_t)(st + k) * dpw + i;
        acc = __fadd_rn(acc, __fmul_rn(w[k], tmp[idx]));
        if (tmpClip) clip |= tmpClip[idx] != 0;
    }
    // Any main-kernel source tap >= white - 1, or the main-kernel result itself >= white - 1: use Area.
    if (tmpClip && (clip || acc >= clipThr)) {
        const float* wx = aWX + (size_t)i * aMaxX;
        const float* wy = aWY + (size_t)j * aMaxY;
        acc = 0.0f;
        for (int ky = 0; ky < aCountY[j]; ++ky) {
            const uint16_t* row = src + ((size_t)(aStartY[j] + ky) * 2 + oy) * srcW + ox;
            float racc = 0.0f;
            for (int kx = 0; kx < aCountX[i]; ++kx)
                racc = __fadd_rn(racc, __fmul_rn(wx[kx], (float)row[(size_t)(aStartX[i] + kx) * 2]));
            acc = __fadd_rn(acc, __fmul_rn(wy[ky], racc));
        }
    }
    // round half away from zero (std::round on the CPU), then clamp to [0, min(maxCode, white)].
    const float r = roundf(acc);
    dst[(size_t)(2 * j + oy) * dstW + 2 * i + ox] = (uint16_t)fminf(fmaxf(r, 0.0f), ceiling);
}

class CudaResampler final : public IResampler {
public:
    explicit CudaResampler(int device) : device_(device) {
        cudaSetDevice(device_);
        cudaStreamCreate(&stream_);
    }
    ~CudaResampler() override {
        cudaSetDevice(device_);
        freeGeometry();
        freeBuffers();
        if (stream_) cudaStreamDestroy(stream_);
    }
    std::string name() const override { return "cuda"; }

    Expected<void> resample(const BayerFrame& in, BayerFrame& out, const ResampleParams& p) override {
        R3DSS_TRY(validateParams(in, p));
        cudaSetDevice(device_);
        R3DSS_TRY(ensureGeometry(in, p));
        R3DSS_TRY(ensureBuffers(in, p));
        out.width = p.outWidth;
        out.height = p.outHeight;
        out.bits = in.bits;
        out.cfa = in.cfa;
        out.black = in.black;
        out.white = in.white;
        out.meta = in.meta;
        out.index = in.index;
        out.data.resize(out.sampleCount());
        if (cudaMemcpyAsync(dSrc_, in.data.data(), in.byteSize(), cudaMemcpyHostToDevice, stream_) != cudaSuccess)
            return makeError(ErrorCode::Internal, "cudaMemcpy H2D failed");
        const uint32_t sph = in.height / 2, dpw = p.outWidth / 2, dph = p.outHeight / 2;
        const float clipThr = static_cast<float>(in.white) - 1.0f;
        const bool clipAware = p.clipAware && p.kernel != ResampleKernel::Area;
        const float ceiling = static_cast<float>(outputCeiling(in));
        for (int plane = 0; plane < 4; ++plane) {
            const uint32_t ox = planeOffsetX(plane), oy = planeOffsetY(plane);
            const DevTaps& tx = taps_[plane][0];
            const DevTaps& ty = taps_[plane][1];
            const DevTaps& ax = areaTaps_[plane][0];
            const DevTaps& ay = areaTaps_[plane][1];
            dim3 blk(32, 8);
            dim3 gridH((dpw + blk.x - 1) / blk.x, (sph + blk.y - 1) / blk.y);
            kHorizontal<<<gridH, blk, 0, stream_>>>(dSrc_, in.width, ox, oy, sph, tx.start, tx.count, tx.weights, tx.maxTaps,
                                                    dpw, clipThr, dTmp_, clipAware ? dTmpClip_ : nullptr);
            dim3 gridV((dpw + blk.x - 1) / blk.x, (dph + blk.y - 1) / blk.y);
            kVertical<<<gridV, blk, 0, stream_>>>(dTmp_, clipAware ? dTmpClip_ : nullptr, dpw, dph, ty.start, ty.count,
                                                  ty.weights, ty.maxTaps, dSrc_, in.width, ox, oy, ax.start, ax.count,
                                                  ax.weights, ax.maxTaps, ay.start, ay.count, ay.weights, ay.maxTaps,
                                                  clipThr, ceiling, dDst_, p.outWidth);
        }
        if (cudaMemcpyAsync(out.data.data(), dDst_, out.byteSize(), cudaMemcpyDeviceToHost, stream_) != cudaSuccess)
            return makeError(ErrorCode::Internal, "cudaMemcpy D2H failed");
        if (cudaStreamSynchronize(stream_) != cudaSuccess)
            return makeError(ErrorCode::Internal, std::string("CUDA error: ") + cudaGetErrorString(cudaGetLastError()));
        return {};
    }

private:
    static Expected<void> upload(const TapTable& t, DevTaps& d) {
        d.len = t.dstPlaneLen;
        d.maxTaps = t.maxTaps;
        if (cudaMalloc(&d.start, t.start.size() * sizeof(int32_t)) != cudaSuccess ||
            cudaMalloc(&d.count, t.count.size() * sizeof(uint16_t)) != cudaSuccess ||
            cudaMalloc(&d.weights, t.weights.size() * sizeof(float)) != cudaSuccess)
            return makeError(ErrorCode::Internal, "cudaMalloc taps failed");
        cudaMemcpy(d.start, t.start.data(), t.start.size() * sizeof(int32_t), cudaMemcpyHostToDevice);
        cudaMemcpy(d.count, t.count.data(), t.count.size() * sizeof(uint16_t), cudaMemcpyHostToDevice);
        cudaMemcpy(d.weights, t.weights.data(), t.weights.size() * sizeof(float), cudaMemcpyHostToDevice);
        return {};
    }
    static void release(DevTaps& d) {
        cudaFree(d.start);
        cudaFree(d.count);
        cudaFree(d.weights);
        d = DevTaps{};
    }
    void freeGeometry() {
        for (auto& pl : taps_) for (auto& t : pl) release(t);
        for (auto& pl : areaTaps_) for (auto& t : pl) release(t);
        gw_ = gh_ = gdw_ = gdh_ = 0;
    }
    void freeBuffers() {
        cudaFree(dSrc_); cudaFree(dDst_); cudaFree(dTmp_); cudaFree(dTmpClip_);
        dSrc_ = dDst_ = nullptr; dTmp_ = nullptr; dTmpClip_ = nullptr;
        bw_ = bh_ = bdw_ = bdh_ = 0;
    }
    Expected<void> ensureGeometry(const BayerFrame& in, const ResampleParams& p) {
        if (gw_ == in.width && gh_ == in.height && gdw_ == p.outWidth && gdh_ == p.outHeight && gk_ == p.kernel) return {};
        freeGeometry();
        for (int plane = 0; plane < 4; ++plane) {
            AxisTaps x = buildAxisTaps(in.width, p.outWidth, planeOffsetX(plane), p.kernel);
            AxisTaps y = buildAxisTaps(in.height, p.outHeight, planeOffsetY(plane), p.kernel);
            R3DSS_TRY(upload(x.main, taps_[plane][0]));
            R3DSS_TRY(upload(y.main, taps_[plane][1]));
            R3DSS_TRY(upload(x.area, areaTaps_[plane][0]));
            R3DSS_TRY(upload(y.area, areaTaps_[plane][1]));
        }
        gw_ = in.width; gh_ = in.height; gdw_ = p.outWidth; gdh_ = p.outHeight; gk_ = p.kernel;
        return {};
    }
    Expected<void> ensureBuffers(const BayerFrame& in, const ResampleParams& p) {
        if (bw_ == in.width && bh_ == in.height && bdw_ == p.outWidth && bdh_ == p.outHeight) return {};
        freeBuffers();
        const size_t tmpN = static_cast<size_t>(in.height / 2) * (p.outWidth / 2);
        if (cudaMalloc(&dSrc_, in.byteSize()) != cudaSuccess ||
            cudaMalloc(&dDst_, static_cast<size_t>(p.outWidth) * p.outHeight * sizeof(uint16_t)) != cudaSuccess ||
            cudaMalloc(&dTmp_, tmpN * sizeof(float)) != cudaSuccess || cudaMalloc(&dTmpClip_, tmpN) != cudaSuccess)
            return makeError(ErrorCode::Internal, "cudaMalloc frame buffers failed");
        bw_ = in.width; bh_ = in.height; bdw_ = p.outWidth; bdh_ = p.outHeight;
        return {};
    }

    int device_;
    cudaStream_t stream_ = nullptr;
    DevTaps taps_[4][2], areaTaps_[4][2];
    uint32_t gw_ = 0, gh_ = 0, gdw_ = 0, gdh_ = 0;
    ResampleKernel gk_ = ResampleKernel::Lanczos3;
    uint16_t* dSrc_ = nullptr;
    uint16_t* dDst_ = nullptr;
    float* dTmp_ = nullptr;
    uint8_t* dTmpClip_ = nullptr;
    uint32_t bw_ = 0, bh_ = 0, bdw_ = 0, bdh_ = 0;
};

} // namespace

std::unique_ptr<IResampler> makeCudaResampler(int device) {
    if (device < 0 || device >= cudaDeviceCount()) return nullptr;
    return std::make_unique<CudaResampler>(device);
}

} // namespace r3dss::resample
