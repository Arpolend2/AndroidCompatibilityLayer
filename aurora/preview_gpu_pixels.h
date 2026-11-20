#ifndef ANDROID_COMPATIBILITY_LAYER_PREVIEW_GPU_PIXELS_H
#define ANDROID_COMPATIBILITY_LAYER_PREVIEW_GPU_PIXELS_H

#include <cstddef>
#include <cstdint>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

// GL supplies tightly packed RGBA rows starting from the bottom.
inline void copyReadbackRgbaToBgra(const uint8_t *rgba, uint8_t *bgra,
                                  int width, int height, int stride)
{
#if defined(__aarch64__)
    const uint8_t indices[] = {2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15};
    const uint8x16_t shuffle = vld1q_u8(indices);
#endif
    const std::size_t rowBytes = std::size_t(width) * 4;
    for (int y = 0; y < height; ++y) {
        const uint8_t *src = rgba + std::size_t(height - 1 - y) * rowBytes;
        uint8_t *dst = bgra + std::size_t(y) * stride;
        int x = 0;
#if defined(__aarch64__)
        for (; x <= width - 4; x += 4, src += 16, dst += 16)
            vst1q_u8(dst, vqtbl1q_u8(vld1q_u8(src), shuffle));
#endif
        for (; x < width; ++x, src += 4, dst += 4) {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = src[3];
        }
    }
}

#endif
