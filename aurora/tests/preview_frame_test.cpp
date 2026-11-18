#include "../preview_frame.h"

#include <QPainter>

#include <climits>
#include <cstdio>
#include <cstring>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

static QImage render(const QImage &frame, int width, int height)
{
    QImage texture(width, height, QImage::Format_ARGB32_Premultiplied);
    texture.fill(Qt::transparent);
    {
        QPainter painter(&texture);
        painter.fillRect(texture.rect(), Qt::black);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(QRectF(0, 0, width, height), frame);
    }
    QImage scene(width, height, QImage::Format_ARGB32_Premultiplied);
    scene.fill(0xff0000ff);
    QPainter painter(&scene);
    painter.drawImage(0, 0, texture);
    return scene;
}

int main()
{
    const QRgb source[] = {
        0xff008000, 0x80008000, 0x12345678,
        0x00000000, 0xff102030, 0x87654321
    };
    const QRgb original[] = {
        0xff008000, 0x80008000, 0x12345678,
        0x00000000, 0xff102030, 0x87654321
    };
    const QImage frame = previewCopyFrame(source, 2, 2, 12);
    CHECK(!frame.isNull());
    CHECK(frame.pixel(0, 0) == 0xff008000);
    CHECK(frame.pixel(1, 0) == 0xff008000);
    CHECK(frame.pixel(0, 1) == 0xff000000);
    CHECK(frame.pixel(1, 1) == 0xff102030);
    CHECK(std::memcmp(source, original, sizeof(source)) == 0);

    const QImage scene = render(frame, 2, 2);
    CHECK(scene.pixel(0, 0) == 0xff008000);
    CHECK(scene.pixel(1, 0) == 0xff008000);
    CHECK(scene.pixel(0, 1) == 0xff000000);
    CHECK(scene.pixel(1, 1) == 0xff102030);
    const QImage scaled = render(frame, 4, 4);
    CHECK(scaled.pixel(0, 0) == 0xff008000);
    CHECK(scaled.pixel(3, 0) == 0xff008000);
    CHECK(scaled.pixel(0, 3) == 0xff000000);
    CHECK(scaled.pixel(3, 3) == 0xff102030);

    const QRgb differentAlpha[] = { 0x80008000, 0x80008000 };
    const QRgb opaque[] = { 0xff008000, 0xff008000 };
    CHECK(previewCopyFrame(differentAlpha, 2, 1, 8) == previewCopyFrame(opaque, 2, 1, 8));
    CHECK(previewCopyFrame(nullptr, 2, 2, 12).isNull());
    CHECK(previewCopyFrame(source, 0, 2, 12).isNull());
    CHECK(previewCopyFrame(source, 2, 0, 12).isNull());
    CHECK(previewCopyFrame(source, 2, 2, 7).isNull());
    CHECK(previewCopyFrame(source, INT_MAX, 1, INT_MAX).isNull());
    std::puts("Qt preview frame OK");
    return 0;
}
