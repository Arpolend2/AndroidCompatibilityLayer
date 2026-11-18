#include "preview_image.h"

#include <QBuffer>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QIODevice>

#include <climits>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace {

bool validImageBuffer(const void *pixels, int width, int height, int stride)
{
    if (!pixels || width <= 0 || height <= 0 || width > INT_MAX / 4 ||
        stride < width * 4)
        return false;
    const size_t lastRow = size_t(height - 1);
    const size_t limit = size_t(std::numeric_limits<std::ptrdiff_t>::max());
    return size_t(width) * 4 <= limit &&
           lastRow <= (limit - size_t(width) * 4) / size_t(stride);
}

bool inputBytes(const void *data, size_t size, QByteArray *bytes)
{
    if (!data || !size || size > size_t(INT_MAX)) return false;
    *bytes = QByteArray::fromRawData(static_cast<const char *>(data), int(size));
    return true;
}

bool MEGLER_CB imageSize(void *, const void *data, size_t size, int *width, int *height)
{
    QByteArray bytes;
    if (!width || !height || !inputBytes(data, size, &bytes)) return false;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::ReadOnly)) return false;
    QImageReader reader(&buffer);
    reader.setAutoTransform(false);
    if (!reader.canRead()) return false;
    const QSize dimensions = reader.size();
    if (dimensions.width() <= 0 || dimensions.height() <= 0) return false;
    *width = dimensions.width();
    *height = dimensions.height();
    return true;
}

bool MEGLER_CB imageDecode(void *, const void *data, size_t size, void *pixels,
                           int width, int height, int stride)
{
    QByteArray bytes;
    if (!validImageBuffer(pixels, width, height, stride) ||
        !inputBytes(data, size, &bytes)) return false;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::ReadOnly)) return false;
    QImageReader reader(&buffer);
    reader.setAutoTransform(false);
    QImage image = reader.read();
    if (image.isNull()) return false;
    if (image.width() != width || image.height() != height)
        image = image.scaled(width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) return false;

    unsigned char *output = static_cast<unsigned char *>(pixels);
    for (int y = 0; y < height; ++y) {
        const QRgb *source = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        unsigned char *row = output + size_t(y) * size_t(stride);
        for (int x = 0; x < width; ++x) {
            const QRgb p = source[x];
            row[4 * size_t(x)] = qBlue(p);
            row[4 * size_t(x) + 1] = qGreen(p);
            row[4 * size_t(x) + 2] = qRed(p);
            row[4 * size_t(x) + 3] = qAlpha(p);
        }
    }
    return true;
}

class SinkDevice : public QIODevice {
public:
    SinkDevice(megler_gfx_write write, void *sink) : write_(write), sink_(sink), failed_(false) {}
    bool isSequential() const override { return true; }
    bool failed() const { return failed_; }

protected:
    qint64 readData(char *, qint64) override { return -1; }
    qint64 writeData(const char *data, qint64 size) override
    {
        if (failed_ || size < 0 || quint64(size) > quint64(std::numeric_limits<size_t>::max())) {
            failed_ = true;
            return -1;
        }
        if (size && !write_(sink_, data, size_t(size))) {
            failed_ = true;
            return -1;
        }
        return size;
    }

private:
    megler_gfx_write write_;
    void *sink_;
    bool failed_;
};

bool MEGLER_CB imageEncode(void *, const void *pixels, int width, int height, int stride,
                           int format, int quality, megler_gfx_write write, void *sink)
{
    if (!validImageBuffer(pixels, width, height, stride) || !write ||
        (format != MEGLER_GFX_PNG && format != MEGLER_GFX_JPEG) ||
        (format == MEGLER_GFX_JPEG && (quality < 0 || quality > 100)))
        return false;

    QImage image(width, height, format == MEGLER_GFX_PNG
                 ? QImage::Format_ARGB32_Premultiplied : QImage::Format_RGB32);
    if (image.isNull()) return false;
    const unsigned char *input = static_cast<const unsigned char *>(pixels);
    for (int y = 0; y < height; ++y) {
        const unsigned char *row = input + size_t(y) * size_t(stride);
        QRgb *destination = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < width; ++x) {
            const unsigned char *p = row + 4 * size_t(x);
            destination[x] = qRgba(p[2], p[1], p[0], format == MEGLER_GFX_PNG ? p[3] : 255);
        }
    }

    SinkDevice device(write, sink);
    if (!device.open(QIODevice::WriteOnly)) return false;
    QImageWriter writer(&device, format == MEGLER_GFX_PNG ? "png" : "jpeg");
    if (format == MEGLER_GFX_JPEG) writer.setQuality(quality);
    return writer.write(image) && !device.failed();
}

}

void previewInstallImageCallbacks(megler_gfx_host *host)
{
    if (!host) return;
    host->image_size = imageSize;
    host->image_decode = imageDecode;
    host->image_encode = imageEncode;
}
