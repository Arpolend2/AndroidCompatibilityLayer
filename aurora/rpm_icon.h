#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_RPM_ICON_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_RPM_ICON_H

#include <QByteArray>
#include <QImage>

namespace RpmIcon {
QByteArray png(const QImage &source, int size);
}

#endif
