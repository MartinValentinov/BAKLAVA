#include "jpeg.hpp"

#include <cstdio>
#include <csetjmp>
#include <vector>

extern "C" {
#include <jpeglib.h>
#include <jerror.h>
}

namespace {

struct JpegErr {
    jpeg_error_mgr pub;
    jmp_buf        jump;
    char           msg[JMSG_LENGTH_MAX];
};

void onError(j_common_ptr cinfo) {
    JpegErr* e = reinterpret_cast<JpegErr*>(cinfo->err);
    (*cinfo->err->format_message)(cinfo, e->msg);
    longjmp(e->jump, 1);
}

bool encode(const uint8_t* data, int w, int h, size_t stride,
            int components, J_COLOR_SPACE cs,
            int quality, bool optimize,
            const std::string& path, size_t* outBytes) {
    if (w <= 0 || h <= 0) return false;

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "[jpeg] cannot open %s for writing\n", path.c_str());
        return false;
    }

    jpeg_compress_struct cinfo{};
    JpegErr err{};
    cinfo.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = onError;

    if (setjmp(err.jump)) {
        jpeg_destroy_compress(&cinfo);
        std::fclose(f);
        std::remove(path.c_str());
        std::fprintf(stderr, "[jpeg] libjpeg failed on %s: %s\n", path.c_str(), err.msg);
        return false;
    }

    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, f);

    cinfo.image_width      = static_cast<JDIMENSION>(w);
    cinfo.image_height     = static_cast<JDIMENSION>(h);
    cinfo.input_components = components;
    cinfo.in_color_space   = cs;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    cinfo.optimize_coding = optimize ? TRUE : FALSE;
    if (cs == JCS_RGB) {
        cinfo.comp_info[0].h_samp_factor = 2;
        cinfo.comp_info[0].v_samp_factor = 2;
    }

    jpeg_start_compress(&cinfo, TRUE);
    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row = const_cast<JSAMPROW>(
            reinterpret_cast<const JSAMPLE*>(data + size_t(cinfo.next_scanline) * stride));
        jpeg_write_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_compress(&cinfo);

    const long pos = std::ftell(f);
    jpeg_destroy_compress(&cinfo);
    std::fclose(f);

    if (outBytes) *outBytes = pos > 0 ? size_t(pos) : 0;
    return true;
}

}

bool writeJpegGray(const uint8_t* data, int w, int h, size_t stride,
                   int quality, bool optimize,
                   const std::string& path, size_t* outBytes) {
    return encode(data, w, h, stride, 1, JCS_GRAYSCALE, quality, optimize, path, outBytes);
}

bool writeJpegRGB(const uint8_t* rgb, int w, int h, size_t stride,
                  int quality, bool optimize,
                  const std::string& path, size_t* outBytes) {
    return encode(rgb, w, h, stride, 3, JCS_RGB, quality, optimize, path, outBytes);
}
