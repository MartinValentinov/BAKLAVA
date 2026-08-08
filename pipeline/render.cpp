#include "render.hpp"
#include "util.hpp"

#include <nvjpeg.h>
#include <gdal_priv.h>

#include <fstream>
#include <vector>

namespace {

bool encodeWithGDAL(const uint8_t* dRGB, int W, int H, int quality,
                    const std::string& path) {
    std::fprintf(stderr, "[jpeg] falling back to host libjpeg via GDAL\n");

    const size_t n = size_t(W) * H * 3;
    std::vector<uint8_t> host(n);
    if (cudaMemcpy(host.data(), dRGB, n, cudaMemcpyDeviceToHost) != cudaSuccess)
        return false;

    GDALDriver* memDrv = GetGDALDriverManager()->GetDriverByName("MEM");
    GDALDriver* jpgDrv = GetGDALDriverManager()->GetDriverByName("JPEG");
    if (!memDrv || !jpgDrv) return false;

    GDALDataset* ds = memDrv->Create("", W, H, 3, GDT_Byte, nullptr);
    if (!ds) return false;

    // interleaved RGB -> three bands
    for (int b = 0; b < 3; ++b) {
        CPLErr e = ds->GetRasterBand(b + 1)->RasterIO(
            GF_Write, 0, 0, W, H, host.data() + b, W, H, GDT_Byte, 3, size_t(W) * 3, nullptr);
        if (e != CE_None) { GDALClose(ds); return false; }
    }

    char** opts = nullptr;
    opts = CSLSetNameValue(opts, "QUALITY", std::to_string(quality).c_str());
    GDALDataset* out = jpgDrv->CreateCopy(path.c_str(), ds, FALSE, opts, nullptr, nullptr);
    CSLDestroy(opts);
    GDALClose(ds);
    if (!out) return false;
    GDALClose(out);
    return true;
}

}  // namespace

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

    if (nvjpegCreateSimple(&handle) != NVJPEG_STATUS_SUCCESS)
        return encodeWithGDAL(dRGB, W, H, quality, path);

    if (nvjpegEncoderStateCreate(handle, &state, stream) != NVJPEG_STATUS_SUCCESS ||
        nvjpegEncoderParamsCreate(handle, &params, stream) != NVJPEG_STATUS_SUCCESS) {
        cleanup();
        return encodeWithGDAL(dRGB, W, H, quality, path);
    }

    nvjpegEncoderParamsSetQuality(params, quality, stream);
    // 4:2:0 halves the chroma we do not really have -- the render is grey plus
    // red boxes, and box edges stay legible at this subsampling.
    nvjpegEncoderParamsSetSamplingFactors(params, NVJPEG_CSS_420, stream);
    nvjpegEncoderParamsSetOptimizedHuffman(params, 0, stream);

    nvjpegImage_t img{};
    img.channel[0] = const_cast<uint8_t*>(dRGB);
    img.pitch[0]   = size_t(W) * 3;

    nvjpegStatus_t st = nvjpegEncodeImage(handle, state, params, &img,
                                          NVJPEG_INPUT_RGBI, W, H, stream);
    if (st != NVJPEG_STATUS_SUCCESS) {
        std::fprintf(stderr, "[jpeg] nvjpegEncodeImage failed (%d)\n", int(st));
        cleanup();
        return encodeWithGDAL(dRGB, W, H, quality, path);
    }

    size_t len = 0;
    if (nvjpegEncodeRetrieveBitstream(handle, state, nullptr, &len, stream)
            != NVJPEG_STATUS_SUCCESS) {
        cleanup();
        return encodeWithGDAL(dRGB, W, H, quality, path);
    }

    std::vector<uint8_t> jpg(len);
    if (nvjpegEncodeRetrieveBitstream(handle, state, jpg.data(), &len, stream)
            != NVJPEG_STATUS_SUCCESS) {
        cleanup();
        return encodeWithGDAL(dRGB, W, H, quality, path);
    }
    CUDA_CHECK(cudaStreamSynchronize(stream));

    std::ofstream f(path, std::ios::binary);
    if (!f) { cleanup(); return false; }
    f.write(reinterpret_cast<const char*>(jpg.data()), std::streamsize(len));
    f.close();

    cleanup();
    std::fprintf(stderr, "[jpeg] wrote %s (%.1f MB)\n", path.c_str(), len / 1e6);
    return true;
}
