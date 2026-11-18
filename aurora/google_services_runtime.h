#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_GOOGLE_SERVICES_RUNTIME_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_GOOGLE_SERVICES_RUNTIME_H

#include "../core/include/megler.h"
#include "../core/src/apk/mg_apk.h"

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QString>

class GoogleServicesRuntime
{
public:
    explicit GoogleServicesRuntime(const QString &androidRoot)
        : m_androidRoot(androidRoot) {}

    // Call after megler_init(); the returned paths remain alive for this object's lifetime.
    bool apply(megler_run_config &run, QString *error = nullptr)
    {
        run.gms_apk_path = nullptr;
        run.gms_splits_dir = nullptr;
        run.gsf_apk_path = nullptr;
        run.gms_package = nullptr;
        m_gmsApk.clear();
        m_gmsPackage.clear();
        if (error) error->clear();

        QSettings settings;
        const QString provider = settings.value(QStringLiteral("googleServices/provider"),
                                                QStringLiteral("none")).toString();
        if (provider == QStringLiteral("none")) return true;
        if (provider != QStringLiteral("microg")) {
            return fail(error, QStringLiteral("Unknown Google services provider setting."));
        }

        QString gmsPath = settings.value(QStringLiteral("googleServices/gmsApkPath")).toString();
        if (gmsPath.isEmpty()) {
            gmsPath = QDir(m_androidRoot).filePath(QStringLiteral("services/GmsCore.apk"));
            const QFileInfo bundled(gmsPath);
            if (!bundled.isFile() || !bundled.isReadable())
                return fail(error, QStringLiteral("Bundled microG is not available; select a trusted microG APK."));
        }
        QString gmsPackage;
        bool isMicroG = false;
        if (!servicePackage(gmsPath, &gmsPackage, &isMicroG, error)) return false;

        if (!isMicroG || !(gmsPackage == QStringLiteral("com.google.android.gms") ||
                           gmsPackage.endsWith(QStringLiteral(".android.gms")))) {
            return fail(error, QStringLiteral("The selected APK is not a supported microG services package."));
        }

        m_gmsApk = gmsPath.toUtf8();
        if (gmsPackage != QStringLiteral("com.google.android.gms"))
            m_gmsPackage = gmsPackage.toUtf8();
        run.gms_apk_path = m_gmsApk.constData();
        run.gms_package = m_gmsPackage.isEmpty() ? nullptr : m_gmsPackage.constData();
        return true;
    }

private:
    static bool fail(QString *error, const QString &message)
    {
        if (error) *error = message;
        return false;
    }

    static bool servicePackage(const QString &path, QString *package, bool *microG, QString *error)
    {
        const QFileInfo file(path);
        if (path.isEmpty() || !file.isFile() || !file.isReadable())
            return fail(error, QStringLiteral("A selected Google services APK is missing or unreadable."));

        const QByteArray pathBytes = path.toUtf8();
        mg_apk_info info = {};
        if (!mg_apk_read_info(pathBytes.constData(), &info)) {
            const QString detail = QString::fromUtf8(info.error);
            return fail(error, detail.isEmpty() ? QStringLiteral("Could not inspect a selected Google services APK.")
                                                : QStringLiteral("Could not inspect a selected Google services APK: %1").arg(detail));
        }
        if (info.split[0] || info.config_for_split[0] || info.feature_split ||
            info.split_required || info.required_split_types[0])
            return fail(error, QStringLiteral("Split microG APKs are not supported."));

        *package = QString::fromUtf8(info.package);
        *microG = mg_apk_gms_name(info.package) && mg_apk_is_microg(pathBytes.constData());
        return true;
    }

    QString m_androidRoot;
    QByteArray m_gmsApk;
    QByteArray m_gmsPackage;
};

#endif
