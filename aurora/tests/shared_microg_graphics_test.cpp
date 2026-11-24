#include "../preview_text.h"

#include <QGuiApplication>
#include <QFont>
#include <QRawFont>
#include <cstdio>
#include <cstdlib>
#include <thread>

extern "C" int test_worker_graphics(const megler_gfx_host *);
extern "C" void *mg_alloc(size_t n) {
    void *p = std::calloc(1, n ? n : 1);
    if (!p) std::abort();
    return p;
}
extern "C" void *mg_realloc(void *old, size_t n) {
    void *p = std::realloc(old, n ? n : 1);
    if (!p) std::abort();
    return p;
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    const megler_gfx_host host = previewGraphics();
    megler_gfx_paint paint = {};
    paint.text_size = 20;
    paint.text_scale_x = 1;
    const uint16_t narrow[] = {'I'}, wide[] = {'W'};
    const float narrowWidth = host.text_measure(host.user, narrow, 1, &paint, nullptr);
    const float wideWidth = host.text_measure(host.user, wide, 1, &paint, nullptr);
    if (wideWidth <= narrowWidth * 1.5f) {
        const QRawFont raw = QRawFont::fromFont(QFont());
        const QVector<quint32> glyphs = raw.glyphIndexesForString(QStringLiteral("IW"));
        std::fprintf(stderr, "Minimal Qt font host cannot measure real glyphs: I=%f W=%f valid=%d ids=%u,%u\n",
                     narrowWidth, wideWidth, raw.isValid(), glyphs.size() > 0 ? glyphs[0] : 0,
                     glyphs.size() > 1 ? glyphs[1] : 0);
        return 1;
    }
    int result = 0;
    std::thread render([&]() { result = test_worker_graphics(&host); });
    render.join();
    if (result) {
        std::fprintf(stderr, "Worker graphics fixture failed: %d\n", result);
        return result;
    }
    std::puts("Worker graphics and pixel callback passed");
    return 0;
}
