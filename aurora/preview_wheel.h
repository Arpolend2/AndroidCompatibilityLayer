#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_PREVIEW_WHEEL_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_PREVIEW_WHEEL_H

#include <QPointF>
#include <QRectF>
#include <QtGlobal>

inline bool previewWheelGesture(const QRectF &target, const QPointF &point,
                                bool horizontal, qreal direction, QPointF *end)
{
    if (!target.contains(point) || direction == 0) return false;
    const qreal position = horizontal ? point.x() : point.y();
    const qreal low = horizontal ? target.left() : target.top();
    const qreal high = horizontal ? target.right() : target.bottom();
    const qreal length = qBound(qreal(24), qAbs(direction) * 64, qreal(64));
    const qreal distance = direction > 0 ? qMin(length, high - position)
                                         : -qMin(length, position - low);
    if (qAbs(distance) < 24) return false;
    *end = point + (horizontal ? QPointF(distance, 0) : QPointF(0, distance));
    return true;
}

#endif
