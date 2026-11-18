#include "rpm_icon.h"

#include <QBuffer>
#include <QPainter>

QByteArray RpmIcon::png(const QImage &source, int size)
{
    if (source.isNull() || size <= 0) return QByteArray();
    QImage canvas(size, size, QImage::Format_ARGB32_Premultiplied);
    if (canvas.isNull()) return QByteArray();
    canvas.fill(Qt::white);
    const int safeSize = qMax(1, size * 4 / 5);
    const QImage scaled = source.scaled(safeSize, safeSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QPainter painter(&canvas);
    painter.drawImage((size - scaled.width()) / 2, (size - scaled.height()) / 2, scaled);
    painter.end();
    QByteArray result;
    QBuffer buffer(&result);
    if (!buffer.open(QIODevice::WriteOnly) || !canvas.save(&buffer, "PNG")) return QByteArray();
    return result;
}
