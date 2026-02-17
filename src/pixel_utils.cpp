#include "pixel_utils.h"

#include <cstring>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define USE_NEON_OPTIMIZATION 1
#else
#define USE_NEON_OPTIMIZATION 0
#endif

namespace ili9488::pixel {

void ConvertRgb888ToRgb666(const uint8_t* src, uint8_t* dst, size_t pixel_count) {
    for (size_t i = 0; i < pixel_count; ++i) {
        const uint8_t r = src[i * 3 + 0] & 0xFC;
        const uint8_t g = src[i * 3 + 1] & 0xFC;
        const uint8_t b = src[i * 3 + 2] & 0xFC;
        dst[i * 3 + 0] = r;
        dst[i * 3 + 1] = g;
        dst[i * 3 + 2] = b;
    }
}

void ConvertRgba8888ToRgb666(const uint8_t* src, uint8_t* dst, size_t pixel_count) {
    for (size_t i = 0; i < pixel_count; ++i) {
        const uint8_t r = src[i * 4 + 0] & 0xFC;
        const uint8_t g = src[i * 4 + 1] & 0xFC;
        const uint8_t b = src[i * 4 + 2] & 0xFC;
        dst[i * 3 + 0] = r;
        dst[i * 3 + 1] = g;
        dst[i * 3 + 2] = b;
    }
}

void ConvertRgb888ToRgb565(const uint8_t* src, uint8_t* dst, size_t pixel_count) {
    for (size_t i = 0; i < pixel_count; ++i) {
        const uint8_t r = src[i * 3 + 0];
        const uint8_t g = src[i * 3 + 1];
        const uint8_t b = src[i * 3 + 2];
        const uint16_t value = static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        dst[i * 2 + 0] = static_cast<uint8_t>(value >> 8);
        dst[i * 2 + 1] = static_cast<uint8_t>(value & 0xFF);
    }
}

void ConvertRgba8888ToRgb565(const uint8_t* src, uint8_t* dst, size_t pixel_count) {
    for (size_t i = 0; i < pixel_count; ++i) {
        const uint8_t r = src[i * 4 + 0];
        const uint8_t g = src[i * 4 + 1];
        const uint8_t b = src[i * 4 + 2];
        const uint16_t value = static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        dst[i * 2 + 0] = static_cast<uint8_t>(value >> 8);
        dst[i * 2 + 1] = static_cast<uint8_t>(value & 0xFF);
    }
}

namespace {

void Rotate180Optimized(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height) {
    const size_t row_bytes = static_cast<size_t>(width) * 3;
    const size_t total_pixels = static_cast<size_t>(width) * height;

    const uint8_t* src_end = src + total_pixels * 3;
    uint8_t* dst_ptr = dst;

    const size_t pixels_4 = total_pixels & ~3UL;
    for (size_t i = 0; i < pixels_4; i += 4) {
        const uint8_t* s = src_end - (i + 4) * 3;
        dst_ptr[0]  = s[9];  dst_ptr[1]  = s[10]; dst_ptr[2]  = s[11];
        dst_ptr[3]  = s[6];  dst_ptr[4]  = s[7];  dst_ptr[5]  = s[8];
        dst_ptr[6]  = s[3];  dst_ptr[7]  = s[4];  dst_ptr[8]  = s[5];
        dst_ptr[9]  = s[0];  dst_ptr[10] = s[1];  dst_ptr[11] = s[2];
        dst_ptr += 12;
    }

    for (size_t i = pixels_4; i < total_pixels; ++i) {
        const uint8_t* s = src_end - (i + 1) * 3;
        dst_ptr[0] = s[0];
        dst_ptr[1] = s[1];
        dst_ptr[2] = s[2];
        dst_ptr += 3;
    }
}

void Rotate90Tiled(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height) {
    const uint32_t dst_width = height;
    const uint32_t dst_height = width;
    constexpr uint32_t kTileSize = 8;

    for (uint32_t tile_y = 0; tile_y < height; tile_y += kTileSize) {
        for (uint32_t tile_x = 0; tile_x < width; tile_x += kTileSize) {
            const uint32_t tile_h = (tile_y + kTileSize <= height) ? kTileSize : (height - tile_y);
            const uint32_t tile_w = (tile_x + kTileSize <= width) ? kTileSize : (width - tile_x);

            for (uint32_t y = 0; y < tile_h; ++y) {
                const uint32_t src_y = tile_y + y;
                for (uint32_t x = 0; x < tile_w; ++x) {
                    const uint32_t src_x = tile_x + x;
                    const uint32_t dst_x = dst_width - 1 - src_y;
                    const uint32_t dst_y = src_x;

                    const size_t src_idx = (static_cast<size_t>(src_y) * width + src_x) * 3;
                    const size_t dst_idx = (static_cast<size_t>(dst_y) * dst_width + dst_x) * 3;

                    dst[dst_idx + 0] = src[src_idx + 0];
                    dst[dst_idx + 1] = src[src_idx + 1];
                    dst[dst_idx + 2] = src[src_idx + 2];
                }
            }
        }
    }
}

void Rotate270Tiled(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height) {
    const uint32_t dst_width = height;
    const uint32_t dst_height = width;
    constexpr uint32_t kTileSize = 8;

    for (uint32_t tile_y = 0; tile_y < height; tile_y += kTileSize) {
        for (uint32_t tile_x = 0; tile_x < width; tile_x += kTileSize) {
            const uint32_t tile_h = (tile_y + kTileSize <= height) ? kTileSize : (height - tile_y);
            const uint32_t tile_w = (tile_x + kTileSize <= width) ? kTileSize : (width - tile_x);

            for (uint32_t y = 0; y < tile_h; ++y) {
                const uint32_t src_y = tile_y + y;
                for (uint32_t x = 0; x < tile_w; ++x) {
                    const uint32_t src_x = tile_x + x;
                    const uint32_t dst_x = src_y;
                    const uint32_t dst_y = dst_height - 1 - src_x;

                    const size_t src_idx = (static_cast<size_t>(src_y) * width + src_x) * 3;
                    const size_t dst_idx = (static_cast<size_t>(dst_y) * dst_width + dst_x) * 3;

                    dst[dst_idx + 0] = src[src_idx + 0];
                    dst[dst_idx + 1] = src[src_idx + 1];
                    dst[dst_idx + 2] = src[src_idx + 2];
                }
            }
        }
    }
}

}

void RotateRgb666(const uint8_t* src,
                  uint8_t* dst,
                  uint32_t width,
                  uint32_t height,
                  int rotation_degrees) {
    switch (rotation_degrees) {
        case 0: {
            const size_t bytes = static_cast<size_t>(width) * height * 3;
            std::memcpy(dst, src, bytes);
            return;
        }
        case 90:
            Rotate90Tiled(src, dst, width, height);
            return;
        case 180:
            Rotate180Optimized(src, dst, width, height);
            return;
        case 270:
            Rotate270Tiled(src, dst, width, height);
            return;
        default:
            break;
    }

    const size_t bytes = static_cast<size_t>(width) * height * 3;
    std::memcpy(dst, src, bytes);
}

}
