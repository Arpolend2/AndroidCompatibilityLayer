#include "../preview_viewport.h"
#include "../preview_wheel.h"

#include <cstdio>

int main()
{
    const PreviewViewport portrait = previewViewport(1080, 2340, 1);
    const PreviewViewport landscape = previewViewport(2340, 1080, 1);
    const PreviewViewport scaled = previewViewport(540, 1170, 2);
    const PreviewViewport capped = previewViewport(1080, 2340, 1, 960);
    const PreviewViewport cappedLandscape = previewViewport(2340, 1080, 1, 960);
    const PreviewViewport small = previewViewport(400, 800, 1, 960);
    if (portrait.pixels != QSize(1080, 2340) || portrait.density != 360 ||
        landscape.pixels != QSize(2340, 1080) || landscape.density != portrait.density ||
        scaled.pixels != portrait.pixels || scaled.density != portrait.density ||
        capped.pixels != QSize(443, 960) || capped.density != 148 ||
        cappedLandscape.pixels != QSize(960, 443) || cappedLandscape.density != capped.density ||
        small.pixels != QSize(400, 800) || small.density != previewViewport(400, 800, 1).density) {
        std::fprintf(stderr, "Incorrect preview viewport\n");
        return 1;
    }
    QPointF end;
    const QRectF target(10, 20, 200, 300);
    if (!previewWheelGesture(target, QPointF(80, 100), false, 1, &end) || end != QPointF(80, 164) ||
        !previewWheelGesture(target, QPointF(80, 100), true, -1, &end) || end != QPointF(16, 100) ||
        previewWheelGesture(target, QPointF(80, 310), false, 1, &end) ||
        previewWheelGesture(target, QPointF(0, 100), false, 1, &end) ||
        previewWheelGesture(target, QPointF(80, 100), false, 0, &end)) {
        std::fprintf(stderr, "Incorrect wheel gesture mapping\n");
        return 1;
    }
    return 0;
}
