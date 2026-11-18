#include "../preview_text.h"

#include <QGuiApplication>
#include <QFile>
#include <QRawFont>
#include <QString>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <thread>
#include <vector>

struct FontFixture {
    QByteArray bytes;
    float expected;
    bool regular;
    const char *path;
};

static bool loadFixture(FontFixture &fixture, const megler_gfx_paint &paint, const char *path)
{
    QFile file(QString::fromLocal8Bit(path));
    if (!file.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "Cannot open font: %s\n", path);
        return false;
    }
    fixture.bytes = file.readAll();
    fixture.path = path;
    const QRawFont raw(fixture.bytes, 128);
    const QVector<quint32> glyphs = raw.glyphIndexesForString(QStringLiteral("W"));
    if (!raw.isValid() || glyphs.size() != 1 || !glyphs[0]) {
        std::fprintf(stderr, "Fixture is not a usable font with a W glyph: %s\n", path);
        return false;
    }
    const QVector<QPointF> advances = raw.advancesForGlyphIndexes(glyphs);
    if (advances.size() != 1) return false;
    fixture.expected = float(advances[0].x() * paint.text_size / 128);
    fixture.regular = raw.weight() == QFont::Normal && raw.style() == QFont::StyleNormal;
    return true;
}

static bool testEmbeddedFont(const megler_gfx_host &host, const megler_gfx_paint &paint, const FontFixture &fixture)
{
    QByteArray bytes(fixture.bytes.constData(), fixture.bytes.size());
    void *face = host.typeface_from_data(nullptr, bytes.constData(), size_t(bytes.size()));
    if (!face) {
        std::fprintf(stderr, "Embedded font registration failed: %s\n", fixture.path);
        return false;
    }
    bytes.fill('\0');
    const QString text = QStringLiteral("W");
    const float measured = host.text_measure(nullptr, text.utf16(), text.size(), &paint, face);
    if (std::fabs(measured - fixture.expected) > 0.5f) {
        std::fprintf(stderr, "Embedded font advance differs from fixture: %f vs %f (%s)\n",
                     measured, fixture.expected, fixture.path);
        return false;
    }
    void *derived = host.typeface_derive(nullptr, face, 700, true);
    if (!derived || host.text_measure(nullptr, text.utf16(), text.size(), &paint, derived) <= 0 ||
        std::fabs(host.text_measure(nullptr, text.utf16(), text.size(), &paint, face) - fixture.expected) > 0.5f) {
        std::fprintf(stderr, "Embedded font derivation failed: %s\n", fixture.path);
        return false;
    }
    if (fixture.regular) {
        void *regular = host.typeface_derive(nullptr, face, 400, false);
        if (!regular ||
            std::fabs(host.text_measure(nullptr, text.utf16(), text.size(), &paint, regular) - fixture.expected) > 0.5f) {
            std::fprintf(stderr, "Derived font lost its embedded family: %s\n", fixture.path);
            return false;
        }
    }
    return true;
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    const megler_gfx_host host = previewGraphics();
    megler_gfx_paint paint = {};
    paint.color = 0xff000000;
    paint.style = MEGLER_GFX_FILL;
    paint.text_size = 48;
    paint.text_scale_x = 1;
    paint.blend = MEGLER_GFX_BLEND_SRC_OVER;
    paint.filter_mode = -1;

    if (host.typeface_from_data(nullptr, nullptr, 100) ||
        host.typeface_from_data(nullptr, "bad font", 8) ||
        host.typeface_from_data(nullptr, "x", size_t(std::numeric_limits<int>::max()) + 1)) {
        std::fprintf(stderr, "Invalid embedded font accepted\n");
        return 1;
    }
    if (argc > 1) {
        std::atomic<bool> ok(true);
        std::vector<FontFixture> fixtures(size_t(argc - 1));
        for (int i = 1; i < argc; ++i)
            if (!loadFixture(fixtures[size_t(i - 1)], paint, argv[i])) return 1;
        std::vector<std::thread> workers;
        for (int i = 1; i < argc; ++i)
            workers.emplace_back([&, i] { if (!testEmbeddedFont(host, paint, fixtures[size_t(i - 1)])) ok = false; });
        for (std::thread &worker : workers) worker.join();
        if (!ok) return 1;
    } else {
        std::fprintf(stderr, "Embedded font tests skipped: pass font file paths as arguments\n");
    }

    const QString narrow = QStringLiteral("I"), wide = QStringLiteral("W");
    const float narrowWidth = host.text_measure(nullptr, narrow.utf16(), narrow.size(), &paint, nullptr);
    const float wideWidth = host.text_measure(nullptr, wide.utf16(), wide.size(), &paint, nullptr);
    if (!(wideWidth > narrowWidth * 1.5f)) {
        std::fprintf(stderr, "System font did not produce distinct glyph widths: %f %f\n", narrowWidth, wideWidth);
        return 1;
    }

    const QString run = QString::fromUtf8("A😀B");
    float widths[4];
    host.text_widths(nullptr, run.utf16(), run.size(), &paint, nullptr, widths);
    const float total = widths[0] + widths[1] + widths[2] + widths[3];
    if (widths[2] != 0 || std::fabs(total - host.text_measure(nullptr, run.utf16(), run.size(), &paint, nullptr)) > 0.01f) {
        std::fprintf(stderr, "Text advances do not match the shaped run\n");
        return 1;
    }
    const float firstWidth = host.text_measure(nullptr, run.utf16(), 1, &paint, nullptr);
    if (host.text_break(nullptr, run.utf16(), run.size(), firstWidth, &paint, nullptr, false) != 1) {
        std::fprintf(stderr, "Text break split or included a character that does not fit\n");
        return 1;
    }

    megler_gfx_paint stretched = paint;
    stretched.text_scale_x = 2;
    if (std::fabs(host.text_measure(nullptr, wide.utf16(), wide.size(), &stretched, nullptr) - 2 * wideWidth) > 0.01f) {
        std::fprintf(stderr, "Horizontal text scaling changed the advance incorrectly\n");
        return 1;
    }
    const QString lines = QStringLiteral("A\nB");
    if (host.text_break(nullptr, lines.utf16(), lines.size(), 1000, &paint, nullptr, false) != 2) {
        std::fprintf(stderr, "Text break crossed a newline\n");
        return 1;
    }

    std::vector<uint32_t> pixels(100 * 100, 0xffffffff);
    void *canvas = host.canvas_new(nullptr, pixels.data(), 100, 100, 400);
    if (!canvas) return 1;
    const QString text = QStringLiteral("O");
    float bounds[4];
    host.text_bounds(nullptr, text.utf16(), text.size(), &paint, nullptr, bounds);
    host.draw_text(nullptr, canvas, text.utf16(), text.size(), 20, 70, &paint, nullptr, nullptr);
    host.canvas_free(nullptr, canvas);
    const int left = int(20 + bounds[0]), right = int(20 + bounds[2]);
    const int top = int(70 + bounds[1]), bottom = int(70 + bounds[3]);
    if (left < 0 || top < 0 || right >= 100 || bottom >= 100 || right <= left || bottom <= top) return 1;
    int black = 0, white = 0;
    for (int y = top; y <= bottom; ++y)
        for (int x = left; x <= right; ++x)
            (pixels[y * 100 + x] == 0xffffffff ? white : black)++;
    if (!black || !white || pixels[((top + bottom) / 2) * 100 + (left + right) / 2] != 0xffffffff) {
        std::fprintf(stderr, "Glyph outline did not preserve the counter in O\n");
        return 1;
    }
    std::fill(pixels.begin(), pixels.end(), 0xffffffff);
    canvas = host.canvas_new(nullptr, pixels.data(), 100, 100, 400);
    if (!canvas) return 1;
    paint.flags = MEGLER_GFX_ANTI_ALIAS;
    host.clip_rect(nullptr, canvas, 0, 0, 50, 100, MEGLER_GFX_CLIP_INTERSECT);
    host.draw_text(nullptr, canvas, text.utf16(), text.size(), 20, 70, &paint, nullptr, nullptr);
    host.canvas_free(nullptr, canvas);
    bool partial = false;
    for (int y = 0; y < 100; ++y) {
        for (int x = 0; x < 100; ++x) {
            const uint32_t pixel = pixels[y * 100 + x];
            if (x >= 50 && pixel != 0xffffffff) {
                std::fprintf(stderr, "Antialiased text escaped the clip\n");
                return 1;
            }
            if (pixel != 0xff000000 && pixel != 0xffffffff) partial = true;
        }
    }
    if (!partial) {
        std::fprintf(stderr, "Antialiased text has no partially covered pixels\n");
        return 1;
    }
    std::puts("Qt preview text OK");
    return 0;
}
