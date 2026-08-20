#include "render.hpp"
#include "jpeg.hpp"
#include "util.hpp"

#include <nvjpeg.h>

#include <fstream>
#include <vector>

namespace {

bool encodeOnHost(const uint8_t* dRGB, int W, int H, int quality,
                  const std::string& path) {
    const size_t n = size_t(W) * H * 3;
    std::vector<uint8_t> host;
    try {
        host.resize(n);
    } catch (const std::bad_alloc&) {
        std::fprintf(stderr, "[jpeg] cannot allocate %.2f GB on the host for the "
                             "fallback encode\n", n / 1e9);
        return false;
    }
    if (cudaMemcpy(host.data(), dRGB, n, cudaMemcpyDeviceToHost) != cudaSuccess) {
        std::fprintf(stderr, "[jpeg] device->host copy failed\n");
        return false;
    }
    size_t bytes = 0;
    if (!writeJpegRGB(host.data(), W, H, size_t(W) * 3, quality, false, path, &bytes))
        return false;
    std::fprintf(stderr, "[jpeg] wrote %s on the host (%.1f MB)\n", path.c_str(), bytes / 1e6);
    return true;
}

const char* nvjpegStatusName(nvjpegStatus_t s) {
    switch (s) {
        case NVJPEG_STATUS_SUCCESS:                    return "SUCCESS";
        case NVJPEG_STATUS_NOT_INITIALIZED:            return "NOT_INITIALIZED";
        case NVJPEG_STATUS_INVALID_PARAMETER:          return "INVALID_PARAMETER";
        case NVJPEG_STATUS_BAD_JPEG:                   return "BAD_JPEG";
        case NVJPEG_STATUS_JPEG_NOT_SUPPORTED:         return "JPEG_NOT_SUPPORTED";
        case NVJPEG_STATUS_ALLOCATOR_FAILURE:          return "ALLOCATOR_FAILURE";
        case NVJPEG_STATUS_EXECUTION_FAILED:           return "EXECUTION_FAILED";
        case NVJPEG_STATUS_ARCH_MISMATCH:              return "ARCH_MISMATCH";
        case NVJPEG_STATUS_INTERNAL_ERROR:             return "INTERNAL_ERROR";
        case NVJPEG_STATUS_IMPLEMENTATION_NOT_SUPPORTED: return "IMPLEMENTATION_NOT_SUPPORTED";
        default:                                       return "UNKNOWN";
    }
}

}

bool encodeJpegRGB(const uint8_t* dRGB, int W, int H, int quality,
                   const std::string& path, cudaStream_t stream) {
    nvjpegHandle_t handle{};
    nvjpegEncoderState_t state{};
    nvjpegEncoderParams_t params{};

    auto cleanup = [&]() {
        if (params) nvjpegEncoderParamsDestroy(params);
        if (state)  nvjpegEncoderStateDestroy(state);
        if (handle) nvjpegDestroy(handle);
    };

    nvjpegStatus_t st = nvjpegCreateSimple(&handle);
    if (st != NVJPEG_STATUS_SUCCESS) {
        std::fprintf(stderr, "[jpeg] nvjpegCreateSimple -> %s; encoding on the host\n",
                     nvjpegStatusName(st));
        return encodeOnHost(dRGB, W, H, quality, path);
    }
    if ((st = nvjpegEncoderStateCreate(handle, &state, stream)) != NVJPEG_STATUS_SUCCESS) {
        std::fprintf(stderr, "[jpeg] nvjpegEncoderStateCreate -> %s; encoding on the host\n",
                     nvjpegStatusName(st));
        cleanup();
        return encodeOnHost(dRGB, W, H, quality, path);
    }
    if ((st = nvjpegEncoderParamsCreate(handle, &params, stream)) != NVJPEG_STATUS_SUCCESS) {
        std::fprintf(stderr, "[jpeg] nvjpegEncoderParamsCreate -> %s; encoding on the host\n",
                     nvjpegStatusName(st));
        cleanup();
        return encodeOnHost(dRGB, W, H, quality, path);
    }

    nvjpegEncoderParamsSetQuality(params, quality, stream);
    nvjpegEncoderParamsSetSamplingFactors(params, NVJPEG_CSS_420, stream);
    nvjpegEncoderParamsSetOptimizedHuffman(params, 0, stream);

    nvjpegImage_t img{};
    img.channel[0] = const_cast<uint8_t*>(dRGB);
    img.pitch[0]   = size_t(W) * 3;

    st = nvjpegEncodeImage(handle, state, params, &img, NVJPEG_INPUT_RGBI, W, H, stream);
    if (st != NVJPEG_STATUS_SUCCESS) {
        std::fprintf(stderr, "[jpeg] nvjpegEncodeImage -> %s; encoding on the host\n",
                     nvjpegStatusName(st));
        cleanup();
        return encodeOnHost(dRGB, W, H, quality, path);
    }

    size_t len = 0;
    if ((st = nvjpegEncodeRetrieveBitstream(handle, state, nullptr, &len, stream))
            != NVJPEG_STATUS_SUCCESS) {
        std::fprintf(stderr, "[jpeg] nvjpegEncodeRetrieveBitstream(size) -> %s; "
                             "encoding on the host\n", nvjpegStatusName(st));
        cleanup();
        return encodeOnHost(dRGB, W, H, quality, path);
    }

    std::vector<uint8_t> jpg(len);
    if ((st = nvjpegEncodeRetrieveBitstream(handle, state, jpg.data(), &len, stream))
            != NVJPEG_STATUS_SUCCESS) {
        std::fprintf(stderr, "[jpeg] nvjpegEncodeRetrieveBitstream -> %s; "
                             "encoding on the host\n", nvjpegStatusName(st));
        cleanup();
        return encodeOnHost(dRGB, W, H, quality, path);
    }
    CUDA_CHECK(cudaStreamSynchronize(stream));

    std::ofstream f(path, std::ios::binary);
    if (!f) { cleanup(); return false; }
    f.write(reinterpret_cast<const char*>(jpg.data()), std::streamsize(len));
    f.close();

    cleanup();
    std::fprintf(stderr, "[jpeg] wrote %s on the GPU (%.1f MB)\n", path.c_str(), len / 1e6);
    return true;
}
