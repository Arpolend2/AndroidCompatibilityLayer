#ifndef MEGLER_AURORA_RPM_ICON_H
#define MEGLER_AURORA_RPM_ICON_H

#include <QByteArray>
#include <QImage>

namespace RpmIcon {
QByteArray png(const QImage &source, int size);
}

#endif
