#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_PREVIEW_FRAME_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_PREVIEW_FRAME_H

#include <QImage>

#include <climits>

inline QImage previewCopyFrame(const void *pixels, int width, int height, int stride)
{
    if (!pixels || width <= 0 || height <= 0 || width > INT_MAX / 4 || stride < width * 4)
        return QImage();
    QImage copy = QImage(static_cast<const uchar *>(pixels), width, height, stride,
                         QImage::Format_RGB32).copy();
    if (copy.isNull()) return copy;
    for (int y = 0; y < copy.height(); ++y) {
        QRgb *row = reinterpret_cast<QRgb *>(copy.scanLine(y));
        for (int x = 0; x < copy.width(); ++x)
            row[x] |= 0xff000000u;
    }
    return copy;
}

#endif
