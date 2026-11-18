#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace aurora_camera {

// Copy padded NV12/NV21 planes to tightly packed NV12. The input extents
// include row padding, including the final row.
inline bool copyFrame(int width, int height, const uint8_t *y, size_t yBytes, int yStride,
                      const uint8_t *uv, size_t uvBytes, int uvStride, bool nv21,
                      std::vector<uint8_t> *out, int factor = 1)
{
    if (!out || !y || !uv || width <= 0 || height <= 0 || factor < 1 ||
        (width & 1) || (height & 1) || yStride < width || uvStride < width ||
        width % (2 * factor) || height % (2 * factor))
        return false;
    const size_t h = static_cast<size_t>(height);
    const size_t w = static_cast<size_t>(width);
    if (h > std::numeric_limits<size_t>::max() / static_cast<size_t>(yStride) ||
        h / 2 > std::numeric_limits<size_t>::max() / static_cast<size_t>(uvStride) ||
        h > std::numeric_limits<size_t>::max() / w ||
        w * h > std::numeric_limits<size_t>::max() - w * (h / 2))
        return false;
    if (yBytes < static_cast<size_t>(yStride) * h ||
        uvBytes < static_cast<size_t>(uvStride) * (h / 2))
        return false;
    // factor > 1 keeps every factor-th sample in both directions.
    const size_t f = static_cast<size_t>(factor), ow = w / f, oh = h / f;
    out->resize(ow * oh + ow * (oh / 2));
    for (size_t row = 0; row < oh; ++row)
        for (size_t col = 0; col < ow; ++col)
            (*out)[row * ow + col] = y[row * f * static_cast<size_t>(yStride) + col * f];
    for (size_t row = 0; row < oh / 2; ++row)
        for (size_t col = 0; col < ow; col += 2) {
            const uint8_t *src = uv + row * f * static_cast<size_t>(uvStride) + col * f;
            uint8_t *dst = out->data() + ow * oh + row * ow + col;
            dst[0] = src[nv21 ? 1 : 0];
            dst[1] = src[nv21 ? 0 : 1];
        }
    return true;
}

// Megler's Camera.CameraInfo.orientation: the clockwise turn that makes the sensor picture upright on the display in its
// natural orientation. Qt gives the sensor's clockwise mount angle, so the turn is its complement. Aurora's camera
// providers state the mount angle against a portrait display; where the screen's natural orientation is landscape the app's
// Display.getRotation is zero for landscape, so the turn is taken a quarter further anticlockwise (-1 for an unusable angle).
inline int sensorOrientation(int qtAngle, bool landscapeNative)
{
    if (qtAngle < 0 || qtAngle >= 360 || qtAngle % 90 != 0) return -1;
    return ((360 - qtAngle) % 360 + (landscapeNative ? 270 : 0)) % 360;
}

inline bool copyPlanarFrame(int width, int height, const uint8_t *y, size_t yBytes, int yStride,
                            const uint8_t *u, size_t uBytes, int uStride,
                            const uint8_t *v, size_t vBytes, int vStride,
                            std::vector<uint8_t> *out, int factor = 1)
{
    if (!out || !y || !u || !v || width <= 0 || height <= 0 || factor < 1 ||
        width % (2 * factor) || height % (2 * factor) ||
        (width & 1) || (height & 1) || yStride < width ||
        uStride < width / 2 || vStride < width / 2)
        return false;
    const size_t h = static_cast<size_t>(height), w = static_cast<size_t>(width);
    if (h > std::numeric_limits<size_t>::max() / static_cast<size_t>(yStride) ||
        h / 2 > std::numeric_limits<size_t>::max() / static_cast<size_t>(uStride) ||
        h / 2 > std::numeric_limits<size_t>::max() / static_cast<size_t>(vStride) ||
        h > std::numeric_limits<size_t>::max() / w ||
        w * h > std::numeric_limits<size_t>::max() - w * (h / 2) ||
        yBytes < static_cast<size_t>(yStride) * h ||
        uBytes < static_cast<size_t>(uStride) * (h / 2) ||
        vBytes < static_cast<size_t>(vStride) * (h / 2))
        return false;
    const size_t f = static_cast<size_t>(factor), ow = w / f, oh = h / f;
    out->resize(ow * oh + ow * (oh / 2));
    for (size_t row = 0; row < oh; ++row)
        for (size_t col = 0; col < ow; ++col)
            (*out)[row * ow + col] = y[row * f * static_cast<size_t>(yStride) + col * f];
    for (size_t row = 0; row < oh / 2; ++row)
        for (size_t col = 0; col < ow / 2; ++col) {
            (*out)[ow * oh + row * ow + col * 2] = u[row * f * static_cast<size_t>(uStride) + col * f];
            (*out)[ow * oh + row * ow + col * 2 + 1] = v[row * f * static_cast<size_t>(vStride) + col * f];
        }
    return true;
}

}
