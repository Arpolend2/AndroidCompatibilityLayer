#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_RPM_STANDALONE_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_RPM_STANDALONE_H

#include <QByteArray>
#include <QImage>
#include <QString>
#include <functional>

namespace RpmStandalone {

QByteArray cacheFingerprint(const QString &package, const QString &label, const QImage &icon,
                            const QString &storeRoot, const QString &runtimeRoot,
                            const QString &qmlRoot, const QString &executable, bool fileDiagnostics);
QString cachedRpm(const QString &directory, const QByteArray &fingerprint);
bool saveCache(const QString &directory, const QString &rpm, const QByteArray &fingerprint);
bool reuseOrGenerate(const std::function<QString()> &lookup,
                     const std::function<bool(QString *)> &generate,
                     QString *output, bool *cacheHit);

bool createUnsigned(const QString &package, const QString &label, const QImage &icon,
                    const QString &storeRoot, const QString &runtimeRoot,
                    const QString &qmlRoot, const QString &executable,
                    const QString &outputDirectory, QString *output, QString *error,
                    bool fileDiagnostics = false);

}

#endif
