#include "../rpm_icon.h"

#include <QColor>
#include <QImage>

int main()
{
    QImage wide(200, 100, QImage::Format_ARGB32);
    wide.fill(Qt::red);
    const int sizes[] = {86, 108, 128, 172};
    for (int size : sizes) {
        const QImage icon = QImage::fromData(RpmIcon::png(wide, size), "PNG");
        if (icon.size() != QSize(size, size) ||
            icon.pixelColor(size / 2, size / 2) != QColor(Qt::red) ||
            icon.pixelColor(0, size / 2) != QColor(Qt::white) ||
            icon.pixelColor(size / 2, 0) != QColor(Qt::white))
            return 1;
    }
    QImage tall(100, 200, QImage::Format_ARGB32);
    tall.fill(Qt::blue);
    const QImage icon = QImage::fromData(RpmIcon::png(tall, 172), "PNG");
    if (icon.pixelColor(86, 86) != QColor(Qt::blue) ||
        icon.pixelColor(0, 86) != QColor(Qt::white) ||
        icon.pixelColor(86, 0) != QColor(Qt::white))
        return 1;
    if (!RpmIcon::png(QImage(), 172).isEmpty() || !RpmIcon::png(tall, 0).isEmpty()) return 1;
    return 0;
}
