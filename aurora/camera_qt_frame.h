#pragma once

#include "camera_frame.h"

#include <QVideoFrame>

namespace aurora_camera {

inline bool copyMappedFrame(const QVideoFrame &frame, std::vector<uint8_t> *out, int factor)
{
    const bool planar = frame.pixelFormat() == QVideoFrame::Format_YUV420P ||
                        frame.pixelFormat() == QVideoFrame::Format_YV12;
    if (!planar && frame.pixelFormat() != QVideoFrame::Format_NV12 &&
        frame.pixelFormat() != QVideoFrame::Format_NV21)
        return false;
    const int count = planar ? 3 : 2;
    if (!frame.isMapped() || frame.planeCount() < count || frame.mappedBytes() <= 0 ||
        frame.width() <= 0 || frame.height() <= 0 || (frame.width() & 1) || (frame.height() & 1))
        return false;

    const uint8_t *planes[3] = {};
    size_t sizes[3] = {};
    uintptr_t starts[3] = {};
    size_t remaining = static_cast<size_t>(frame.mappedBytes());
    for (int i = 0; i < count; ++i) {
        const int stride = frame.bytesPerLine(i);
        const int minStride = i == 0 || !planar ? frame.width() : frame.width() / 2;
        const size_t rows = static_cast<size_t>(i == 0 ? frame.height() : frame.height() / 2);
        planes[i] = frame.bits(i);
        if (!planes[i] || stride < minStride ||
            rows > std::numeric_limits<size_t>::max() / static_cast<size_t>(stride))
            return false;
        sizes[i] = rows * static_cast<size_t>(stride);
        if (sizes[i] > remaining) return false;
        remaining -= sizes[i];
        starts[i] = reinterpret_cast<uintptr_t>(planes[i]);
        if (sizes[i] > std::numeric_limits<uintptr_t>::max() - starts[i]) return false;
        for (int j = 0; j < i; ++j) {
            if (starts[i] < starts[j] + sizes[j] && starts[j] < starts[i] + sizes[i])
                return false;
        }
    }

    // Qt reports only the combined mapped size, not individual plane lengths.
    // The buffer provider must supply complete planes for its declared format.
    if (planar) {
        const bool yv12 = frame.pixelFormat() == QVideoFrame::Format_YV12;
        const int u = yv12 ? 2 : 1, v = yv12 ? 1 : 2;
        return copyPlanarFrame(frame.width(), frame.height(), planes[0], sizes[0], frame.bytesPerLine(0),
                               planes[u], sizes[u], frame.bytesPerLine(u),
                               planes[v], sizes[v], frame.bytesPerLine(v), out, factor);
    }
    return copyFrame(frame.width(), frame.height(), planes[0], sizes[0], frame.bytesPerLine(0),
                     planes[1], sizes[1], frame.bytesPerLine(1),
                     frame.pixelFormat() == QVideoFrame::Format_NV21, out, factor);
}

}
