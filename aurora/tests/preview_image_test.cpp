#include "../preview_image.h"

#include <QBuffer>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>

#include <climits>
#include <cstdio>
#include <cstring>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

static bool MEGLER_CB append(void *sink, const void *data, size_t size)
{
    static_cast<QByteArray *>(sink)->append(static_cast<const char *>(data), int(size));
    return true;
}

static bool MEGLER_CB reject(void *sink, const void *, size_t)
{
    ++*static_cast<int *>(sink);
    return false;
}

int main()
{
    megler_gfx_host host = {};
    previewInstallImageCallbacks(&host);
    CHECK(host.image_size && host.image_decode && host.image_encode);

    // Two pixels plus padding: premultiplied BGRA, then an opaque blue pixel.
    const unsigned char pixels[] = { 16, 32, 64, 128, 200, 0, 0, 255, 7, 7, 7, 7 };
    QByteArray png;
    CHECK(host.image_encode(nullptr, pixels, 2, 1, 12, MEGLER_GFX_PNG, -100, append, &png));
    int width = 0, height = 0;
    CHECK(host.image_size(nullptr, png.constData(), png.size(), &width, &height));
    CHECK(width == 2 && height == 1);
    QImage straight = QImage::fromData(png, "PNG").convertToFormat(QImage::Format_ARGB32);
    CHECK(!straight.isNull());
    const QRgb first = straight.pixel(0, 0);
    CHECK(qAlpha(first) == 128 && qRed(first) == 128 && qGreen(first) == 64 && qBlue(first) == 32);
    unsigned char decoded[12];
    std::memset(decoded, 0xaa, sizeof(decoded));
    CHECK(host.image_decode(nullptr, png.constData(), png.size(), decoded, 2, 1, 12));
    CHECK(std::memcmp(decoded, pixels, 8) == 0);
    CHECK(decoded[8] == 0xaa && decoded[11] == 0xaa);

    unsigned char reduced[4] = {};
    CHECK(host.image_decode(nullptr, png.constData(), png.size(), reduced, 1, 1, 4));
    CHECK(reduced[3] > 128 && reduced[3] < 255);
    CHECK(reduced[0] > 16 && reduced[0] < 200);

    const unsigned char translucent[] = { 0, 0, 64, 128 };
    QByteArray jpeg;
    CHECK(host.image_encode(nullptr, translucent, 1, 1, 4, MEGLER_GFX_JPEG, 100, append, &jpeg));
    CHECK(host.image_size(nullptr, jpeg.constData(), jpeg.size(), &width, &height));
    CHECK(width == 1 && height == 1);
    unsigned char opaque[4] = {};
    CHECK(host.image_decode(nullptr, jpeg.constData(), jpeg.size(), opaque, 1, 1, 4));
    CHECK(opaque[3] == 255 && opaque[2] >= 60 && opaque[2] <= 68 && opaque[0] <= 3 && opaque[1] <= 3);

    // Add EXIF orientation 6 to a non-square JPEG.
    QImage rectangle(2, 1, QImage::Format_RGB32);
    rectangle.fill(qRgb(100, 50, 25));
    QByteArray jpegRectangle;
    QBuffer jpegBuffer(&jpegRectangle);
    CHECK(jpegBuffer.open(QIODevice::WriteOnly));
    QImageWriter jpegWriter(&jpegBuffer, "jpeg");
    CHECK(jpegWriter.write(rectangle));
    static const unsigned char exif[] = {
        0xff, 0xe1, 0x00, 0x22, 'E', 'x', 'i', 'f', 0, 0,
        'I', 'I', 42, 0, 8, 0, 0, 0, 1, 0,
        0x12, 0x01, 3, 0, 1, 0, 0, 0, 6, 0, 0, 0, 0, 0, 0, 0
    };
    jpegRectangle.insert(2, QByteArray(reinterpret_cast<const char *>(exif), sizeof(exif)));
    QBuffer orientedBuffer(&jpegRectangle);
    CHECK(orientedBuffer.open(QIODevice::ReadOnly));
    QImageReader orientedReader(&orientedBuffer, "jpeg");
    CHECK(orientedReader.transformation() == QImageIOHandler::TransformationRotate90);
    CHECK(host.image_size(nullptr, jpegRectangle.constData(), jpegRectangle.size(), &width, &height));
    CHECK(width == 2 && height == 1);
    CHECK(host.image_decode(nullptr, jpegRectangle.constData(), jpegRectangle.size(), decoded, 2, 1, 12));

    int calls = 0;
    CHECK(!host.image_encode(nullptr, pixels, 2, 1, 12, MEGLER_GFX_PNG, 100, reject, &calls));
    CHECK(calls == 1);
    CHECK(!host.image_encode(nullptr, pixels, 2, 1, 12, 42, 100, append, &png));
    CHECK(!host.image_encode(nullptr, pixels, 2, 1, 12, MEGLER_GFX_JPEG, 101, append, &png));
    CHECK(!host.image_encode(nullptr, pixels, 2, 1, 7, MEGLER_GFX_PNG, 100, append, &png));
    CHECK(!host.image_encode(nullptr, pixels, INT_MAX, 1, INT_MAX, MEGLER_GFX_PNG, 100, append, &png));
    CHECK(!host.image_encode(nullptr, pixels, 2, 1, 12, MEGLER_GFX_PNG, 100, nullptr, nullptr));
    CHECK(!host.image_decode(nullptr, png.constData(), png.size(), decoded, 2, 1, 7));
    CHECK(!host.image_decode(nullptr, png.constData(), png.size(), decoded, 0, 1, 12));
    CHECK(!host.image_decode(nullptr, "bad", 3, decoded, 2, 1, 12));
    width = 123; height = 456;
    CHECK(!host.image_size(nullptr, "bad", 3, &width, &height));
    CHECK(width == 123 && height == 456);
    CHECK(!host.image_size(nullptr, png.constData(), png.size(), nullptr, &height));
    CHECK(!host.image_size(nullptr, png.constData(), size_t(INT_MAX) + 1, &width, &height));
    std::puts("Qt preview images OK");
    return 0;
}
