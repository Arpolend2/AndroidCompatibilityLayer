#include "preview_text.h"
#include "preview_image.h"

#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QDebug>
#include <QImage>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QPainterPath>
#include <QRawFont>
#include <QTextBoundaryFinder>
#include <QTransform>
#include <QVector>
#include <QtMath>

#include <cmath>
#include <limits>

#include "../core/tests/mg_gfx_standin.h"

namespace {

const int fontSize = 128;

QMutex &fontMutex()
{
    static QMutex mutex(QMutex::Recursive);
    return mutex;
}

struct Face {
    QFont font;
};

int fontWeight(int weight)
{
    if (weight <= 400) return qBound(0, weight * QFont::Normal / 400, int(QFont::Normal));
    if (weight <= 700)
        return QFont::Normal + (weight - 400) * (QFont::Bold - QFont::Normal) / 300;
    return qMin(99, int(QFont::Bold) + (weight - 700) * (99 - int(QFont::Bold)) / 299);
}

QFont fontFor(const megler_gfx_paint *paint, void *typeface)
{
    QFont font = typeface ? static_cast<Face *>(typeface)->font : QFont();
    font.setPixelSize(fontSize);
    font.setLetterSpacing(QFont::AbsoluteSpacing, paint->letter_spacing * fontSize);
    return font;
}

QString printable(const uint16_t *text, int count)
{
    QString result = QString::fromUtf16(text, count);
    for (int i = 0; i < result.size(); ++i) {
        const ushort c = result.at(i).unicode();
        if (c == '\t') result[i] = QLatin1Char(' ');
        else if (c < 0x20 || (c >= 0x7f && c <= 0x9f)) result[i] = QChar(0x200b);
    }
    return result;
}

qreal scale(const megler_gfx_paint *paint)
{
    return qreal(paint->text_size) / fontSize;
}

float measure(const QString &text, const megler_gfx_paint *paint, void *face)
{
    return QFontMetricsF(fontFor(paint, face)).width(text) * scale(paint) * paint->text_scale_x;
}

float MEGLER_CB textMeasure(void *, const uint16_t *text, int count, const megler_gfx_paint *paint, void *face)
{
    QMutexLocker lock(&fontMutex());
    return count > 0 ? measure(printable(text, count), paint, face) : 0;
}

float MEGLER_CB textMetrics(void *, const megler_gfx_paint *paint, void *face, float *out)
{
    QMutexLocker lock(&fontMutex());
    const QFontMetricsF metrics(fontFor(paint, face));
    const qreal s = scale(paint);
    if (out) {
        out[0] = out[1] = -metrics.ascent() * s;
        out[2] = out[3] = metrics.descent() * s;
        out[4] = qMax(qreal(0), metrics.leading() * s);
    }
    return (metrics.ascent() + metrics.descent() + qMax(qreal(0), metrics.leading())) * s;
}

QPainterPath textOutline(const uint16_t *text, int count, float x, float y,
                         const megler_gfx_paint *paint, void *face)
{
    QPainterPath path;
    if (count <= 0) return path;
    const QString string = printable(text, count);
    const float width = measure(string, paint, face);
    if (paint->text_align == MEGLER_GFX_ALIGN_CENTER) x -= width / 2;
    else if (paint->text_align == MEGLER_GFX_ALIGN_RIGHT) x -= width;
    path.addText(0, 0, fontFor(paint, face), string);
    const qreal s = scale(paint);
    return QTransform(s * paint->text_scale_x, 0, s * paint->text_skew_x, s, x, y).map(path);
}

QVector<float> pathStream(const QPainterPath &path)
{
    QVector<float> result;
    bool figure = false;
    for (int i = 0; i < path.elementCount(); ++i) {
        const QPainterPath::Element e = path.elementAt(i);
        if (e.type == QPainterPath::MoveToElement) {
            if (figure) result.append(MEGLER_GFX_PATH_CLOSE);
            result << MEGLER_GFX_PATH_MOVE << float(e.x) << float(e.y);
            figure = true;
        } else if (e.type == QPainterPath::LineToElement) {
            result << MEGLER_GFX_PATH_LINE << float(e.x) << float(e.y);
        } else if (e.type == QPainterPath::CurveToElement && i + 2 < path.elementCount()) {
            const QPainterPath::Element b = path.elementAt(++i);
            const QPainterPath::Element end = path.elementAt(++i);
            result << MEGLER_GFX_PATH_CUBIC << float(e.x) << float(e.y)
                   << float(b.x) << float(b.y) << float(end.x) << float(end.y);
        }
    }
    if (figure) result.append(MEGLER_GFX_PATH_CLOSE);
    return result;
}

void MEGLER_CB textBounds(void *, const uint16_t *text, int count, const megler_gfx_paint *paint, void *face, float *out)
{
    QMutexLocker lock(&fontMutex());
    megler_gfx_paint left = *paint;
    left.text_align = MEGLER_GFX_ALIGN_LEFT;
    const QPainterPath path = textOutline(text, count, 0, 0, &left, face);
    if (path.isEmpty()) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    QRectF box = path.boundingRect();
    if (paint->style != MEGLER_GFX_FILL)
        box.adjust(-paint->stroke_width / 2, -paint->stroke_width / 2,
                   paint->stroke_width / 2, paint->stroke_width / 2);
    out[0] = box.left(); out[1] = box.top(); out[2] = box.right(); out[3] = box.bottom();
}

void MEGLER_CB textWidths(void *, const uint16_t *text, int count, const megler_gfx_paint *paint, void *face, float *out)
{
    QMutexLocker lock(&fontMutex());
    const QString string = printable(text, count);
    for (int i = 0; i < count; ++i) out[i] = 0;
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, string);
    int start = 0;
    float previous = 0;
    while (start < count) {
        const int end = finder.toNextBoundary();
        if (end < 0) break;
        const float width = measure(string.left(end), paint, face);
        out[start] = width - previous;
        previous = width;
        start = end;
    }
}

int MEGLER_CB textBreak(void *, const uint16_t *text, int count, float maxWidth,
                        const megler_gfx_paint *paint, void *face, bool words)
{
    QMutexLocker lock(&fontMutex());
    int line = count;
    for (int i = 0; i < count; ++i) if (text[i] == '\n') { line = i + 1; break; }
    const QString string = printable(text, line);
    QTextBoundaryFinder graphemes(QTextBoundaryFinder::Grapheme, string);
    int fit = 0;
    for (int end = graphemes.toNextBoundary(); end >= 0; end = graphemes.toNextBoundary()) {
        if (measure(string.left(end), paint, face) > maxWidth) break;
        fit = end;
    }
    if (!words || fit == line) return fit;
    QTextBoundaryFinder boundaries(QTextBoundaryFinder::Line, string);
    int best = 0;
    for (int end = boundaries.toNextBoundary(); end >= 0; end = boundaries.toNextBoundary()) {
        int trimmed = end;
        while (trimmed > 0 && string.at(trimmed - 1).isSpace()) --trimmed;
        if (measure(string.left(trimmed), paint, face) > maxWidth) break;
        best = end;
    }
    return best ? best : fit;
}

bool MEGLER_CB textPath(void *, const uint16_t *text, int count, float x, float y,
                        const megler_gfx_paint *paint, void *face, megler_gfx_write write, void *sink)
{
    QMutexLocker lock(&fontMutex());
    const QVector<float> stream = pathStream(textOutline(text, count, x, y, paint, face));
    return stream.isEmpty() || write(sink, stream.constData(), size_t(stream.size()) * sizeof(float));
}

void MEGLER_CB drawText(void *user, void *canvas, const uint16_t *text, int count, float x, float y,
                        const megler_gfx_paint *paint, void *face, void *shader)
{
    QMutexLocker lock(&fontMutex());
    QPainterPath outline = textOutline(text, count, x, y, paint, face);
    if (outline.isEmpty()) return;
    if ((paint->flags & MEGLER_GFX_ANTI_ALIAS) && paint->style == MEGLER_GFX_FILL &&
        paint->blend == MEGLER_GFX_BLEND_SRC_OVER && paint->filter_mode < 0 && !shader) {
        const QRectF bounds = outline.boundingRect();
        const qreal left = std::floor(bounds.left()) - 1;
        const qreal top = std::floor(bounds.top()) - 1;
        const qreal right = std::ceil(bounds.right()) + 1;
        const qreal bottom = std::ceil(bounds.bottom()) + 1;
        const qreal width = right - left, height = bottom - top;
        if (std::isfinite(left) && std::isfinite(top) &&
            width > 0 && height > 0 && width <= 8192 && height <= 8192 &&
            width * height <= 16777216) {
            QImage image(int(width), int(height), QImage::Format_ARGB32_Premultiplied);
            if (!image.isNull()) {
                image.fill(Qt::transparent);
                outline.setFillRule(Qt::WindingFill);
                QPainter painter(&image);
                painter.setRenderHint(QPainter::Antialiasing);
                painter.translate(-left, -top);
                painter.fillPath(outline, QColor::fromRgba(paint->color));
                painter.end();
                const megler_gfx_image source = { image.constBits(), image.width(), image.height(),
                                                  image.bytesPerLine(), 0, 0 };
                const float src[] = { 0, 0, float(width), float(height) };
                const float dst[] = { float(left), float(top), float(right), float(bottom) };
                megler_gfx_paint bitmapPaint = *paint;
                bitmapPaint.color = 0xff000000;
                mg_gfx_standin()->draw_bitmap(user, canvas, &source, src, dst, &bitmapPaint);
                return;
            }
            qWarning() << "Could not allocate antialiased preview text bitmap.";
        }
    }
    const QVector<float> stream = pathStream(outline);
    if (!stream.isEmpty())
        mg_gfx_standin()->draw_path(user, canvas, stream.constData(), stream.size(), MEGLER_GFX_WINDING, paint, shader);
}

void *MEGLER_CB faceCreate(void *, const char *family, int generic, int weight, bool italic)
{
    QMutexLocker lock(&fontMutex());
    Face *face = new Face;
    if (generic == MEGLER_GFX_SERIF) {
        face->font.setFamily(QStringLiteral("serif"));
        face->font.setStyleHint(QFont::Serif);
    } else if (generic == MEGLER_GFX_MONOSPACE) {
        face->font.setFamily(QStringLiteral("monospace"));
        face->font.setStyleHint(QFont::Monospace);
    } else {
        face->font.setFamily(QStringLiteral("sans-serif"));
        face->font.setStyleHint(QFont::SansSerif);
    }
    if (family && family[0] && !QByteArray(family).startsWith("sans-serif") &&
        QByteArray(family) != "serif" && QByteArray(family) != "monospace")
        face->font.setFamily(QString::fromUtf8(family));
    face->font.setWeight(fontWeight(weight));
    face->font.setItalic(italic);
    return face;
}

void *MEGLER_CB faceDerive(void *user, void *typeface, int weight, bool italic)
{
    QMutexLocker lock(&fontMutex());
    Face *face = new Face;
    if (typeface) face->font = static_cast<Face *>(typeface)->font;
    face->font.setStyleName(QString());
    face->font.setWeight(fontWeight(weight));
    face->font.setItalic(italic);
    Q_UNUSED(user);
    return face;
}

void *MEGLER_CB faceFromData(void *, const void *data, size_t size)
{
    QMutexLocker lock(&fontMutex());
    if (!data || !size || size > size_t(std::numeric_limits<int>::max())) {
        qWarning() << "Invalid embedded font data or size.";
        return nullptr;
    }
    const QByteArray bytes(static_cast<const char *>(data), int(size));
    const QRawFont raw(bytes, fontSize);
    if (!raw.isValid()) {
        qWarning() << "Could not read embedded font data.";
        return nullptr;
    }
    const int id = QFontDatabase::addApplicationFontFromData(bytes);
    if (id < 0) {
        qWarning() << "Could not register embedded font with Qt.";
        return nullptr;
    }
    const QStringList families = QFontDatabase::applicationFontFamilies(id);
    if (families.isEmpty()) {
        QFontDatabase::removeApplicationFont(id);
        qWarning() << "Embedded font has no registered family.";
        return nullptr;
    }
    // The host API has no typeface release; Qt retains its copy until process exit.
    Face *face = new Face;
    face->font.setFamily(families.first());
    face->font.setWeight(raw.weight());
    face->font.setStyle(raw.style());
    face->font.setStyleName(raw.styleName());
    return face;
}

}

megler_gfx_host previewGraphics()
{
    megler_gfx_host host = *mg_gfx_standin();
    previewInstallImageCallbacks(&host);
    host.draw_text = drawText;
    host.text_measure = textMeasure;
    host.text_metrics = textMetrics;
    host.text_bounds = textBounds;
    host.text_break = textBreak;
    host.text_widths = textWidths;
    host.text_path = textPath;
    host.typeface_create = faceCreate;
    host.typeface_derive = faceDerive;
    host.typeface_from_data = faceFromData;
    return host;
}
