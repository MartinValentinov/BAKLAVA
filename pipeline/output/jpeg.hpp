#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

bool writeJpegGray(const uint8_t* data, int w, int h, size_t stride,
                   int quality, bool optimize,
                   const std::string& path, size_t* outBytes);

bool writeJpegRGB(const uint8_t* rgb, int w, int h, size_t stride,
                  int quality, bool optimize,
                  const std::string& path, size_t* outBytes);
