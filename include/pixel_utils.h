#pragma once
#include <cstddef>
#include <cstdint>

namespace fbcp::pixel {

void ConvertRgb888ToRgb666(const uint8_t* src, uint8_t* dst, size_t pixel_count);
void ConvertRgba8888ToRgb666(const uint8_t* src, uint8_t* dst, size_t pixel_count);
void ConvertRgb888ToRgb565(const uint8_t* src, uint8_t* dst, size_t pixel_count);
void ConvertRgba8888ToRgb565(const uint8_t* src, uint8_t* dst, size_t pixel_count);
void RotateRgb666(const uint8_t* src,
                  uint8_t* dst,
                  uint32_t width,
                  uint32_t height,
                  int rotation_degrees);

}
