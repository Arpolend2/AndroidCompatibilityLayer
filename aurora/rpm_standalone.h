#ifndef MEGLER_AURORA_RPM_STANDALONE_H
#define MEGLER_AURORA_RPM_STANDALONE_H

#include <QImage>
#include <QString>

namespace RpmStandalone {

bool createUnsigned(const QString &package, const QString &label, const QImage &icon,
                    const QString &storeRoot, const QString &runtimeRoot,
                    const QString &qmlRoot, const QString &executable,
                    const QString &outputDirectory, QString *output, QString *error);

}

#endif
