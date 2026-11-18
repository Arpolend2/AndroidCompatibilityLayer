#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_PREVIEW_VIEWPORT_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_PREVIEW_VIEWPORT_H

#include <QSize>
#include <QtGlobal>

struct PreviewViewport
{
    QSize pixels;
    int density;
};

inline PreviewViewport previewViewport(qreal width, qreal height, qreal pixelRatio, int cap = 0)
{
    int w = qMax(1, qRound(width * pixelRatio));
    int h = qMax(1, qRound(height * pixelRatio));
    const int longer = qMax(w, h);
    if (cap > 0 && longer > cap) {
        const qreal factor = qreal(cap) / longer;
        w = qMax(1, qRound(w * factor));
        h = qMax(1, qRound(h * factor));
    }
    const qreal scale = qMin(qreal(qMin(w, h)) / 480, qreal(qMax(w, h)) / 800);
    return {QSize(w, h), qMax(1, qRound(160 * scale))};
}

#endif
