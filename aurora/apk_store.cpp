#include "apk_store.h"
#include "app_storage_lock.h"

#include <QDir>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QLockFile>
#include <QObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QUuid>

#include "../core/include/megler.h"

#include <algorithm>
#include <limits>

namespace {

bool validPackage(const QString &package)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)+$"));
    return package.size() <= 255 && pattern.match(package).hasMatch();
}

QJsonObject report(const QString &path, const QString &splits, QString *error)
{
    const QByteArray utf8 = path.toUtf8();
    const QByteArray splitUtf8 = splits.toUtf8();
    char *raw = splits.isEmpty() ? megler_apk_report(utf8.constData())
                                 : megler_apk_report_splits(utf8.constData(), splitUtf8.constData());
    if (!raw) {
        *error = QObject::tr("Not enough memory to inspect the APK.");
        return QJsonObject();
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray(raw), &parseError);
    megler_free(raw);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        *error = QObject::tr("Could not read the APK inspection result.");
        return QJsonObject();
    }
    const QJsonObject object = doc.object();
    if (!object.value(QStringLiteral("ok")).toBool()) {
        *error = object.value(QStringLiteral("error")).toString(QObject::tr("Invalid APK."));
        return QJsonObject();
    }
    return object;
}

bool readLaunchCount(const QString &path, quint64 *count, QString *error)
{
    *count = 0;
    if (QFileInfo(path).isSymLink()) {
        *error = QObject::tr("App usage file is a symbolic link.");
        return false;
    }
    QFile file(path);
    if (!file.exists()) return true;
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QObject::tr("Cannot read app usage: %1").arg(file.errorString());
        return false;
    }
    const QByteArray bytes = file.read(64);
    if (file.error() != QFile::NoError || !file.atEnd()) {
        *error = QObject::tr("Cannot read app usage: %1").arg(file.errorString());
        return false;
    }
    bool ok = false;
    *count = QString::fromLatin1(bytes.trimmed()).toULongLong(&ok);
    if (!ok) *error = QObject::tr("Invalid app usage file: %1").arg(path);
    return ok;
}

}

ApkStore::ApkStore(const QString &root) : m_root(root) {}

QString ApkStore::apkPath(const QString &package) const
{
    if (m_root.isEmpty() || !validPackage(package)) return QString();
    return QDir(m_root).filePath(package + QStringLiteral("/base.apk"));
}

QString ApkStore::dataPath(const QString &package) const
{
    if (m_root.isEmpty() || !validPackage(package)) return QString();
    return QDir(m_root).filePath(package + QStringLiteral("/data"));
}

QString ApkStore::lockPath(const QString &package) const
{
    if (m_root.isEmpty() || !validPackage(package)) return QString();
    return appStorageLockPath(m_root, package);
}

bool ApkStore::recordLaunch(const QString &package, QString *message)
{
    const QString apk = apkPath(package);
    const QFileInfo dir(QDir(m_root).filePath(package));
    if (apk.isEmpty() || dir.isSymLink() || !dir.isDir() || !QFileInfo(apk).isFile()) {
        *message = QObject::tr("Cannot record app use: installed app was not found.");
        return false;
    }
    const QString path = QDir(dir.filePath()).filePath(QStringLiteral(".launch-count"));
    quint64 count;
    if (!readLaunchCount(path, &count, message)) return false;
    QSaveFile file(path);
    const QByteArray value = QByteArray::number(count == std::numeric_limits<quint64>::max() ? count : count + 1);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(value) != value.size() ||
        !file.commit()) {
        *message = QObject::tr("Could not save app usage: %1").arg(file.errorString());
        return false;
    }
    return true;
}

bool ApkStore::remove(const QString &package, QString *message)
{
    const QString apk = apkPath(package);
    const QString lockName = lockPath(package);
    if (apk.isEmpty() || lockName.isEmpty()) {
        *message = QObject::tr("Invalid app package.");
        return false;
    }
    QDir root(m_root);
    QLockFile lock(lockName);
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) {
        *message = QObject::tr("The app is running or its data is in use.");
        return false;
    }
    const QFileInfo appDir(root.filePath(package));
    if (appDir.isSymLink() || !appDir.isDir() || !QFileInfo(apk).isFile()) {
        *message = QObject::tr("Installed app was not found.");
        return false;
    }
    const QString removed = QStringLiteral(".removed-") + QUuid::createUuid().toString();
    if (!root.rename(package, removed)) {
        *message = QObject::tr("Could not remove the app from the library.");
        return false;
    }
    if (!QDir(root.filePath(removed)).removeRecursively()) {
        *message = QObject::tr("The app was removed from the library, but some data could not be deleted: %1")
                       .arg(root.filePath(removed));
        return false;
    }
    *message = QObject::tr("Removed %1 and its data.").arg(package);
    return true;
}

bool ApkStore::install(const QString &source, QString *message, QString *installedPackage)
{
    if (installedPackage) installedPackage->clear();
    qInfo("APK library import: stage=open");
    if (m_root.isEmpty()) {
        *message = QObject::tr("App storage is unavailable.");
        return false;
    }
    QFile input(source);
    if (!input.open(QIODevice::ReadOnly)) {
        *message = QObject::tr("Cannot open APK: %1").arg(input.errorString());
        return false;
    }
    if (!QDir().mkpath(m_root)) {
        *message = QObject::tr("Cannot create the app storage directory.");
        return false;
    }
    qInfo("APK library import: stage=copy");
    QTemporaryFile copy(QDir(m_root).filePath(QStringLiteral(".import-XXXXXX")));
    if (!copy.open()) {
        *message = QObject::tr("Cannot create a temporary APK: %1").arg(copy.errorString());
        return false;
    }
    QByteArray buffer(65536, Qt::Uninitialized);
    for (;;) {
        const qint64 size = input.read(buffer.data(), buffer.size());
        if (size < 0) {
            *message = QObject::tr("Cannot read APK: %1").arg(input.errorString());
            return false;
        }
        if (!size) break;
        if (copy.write(buffer.constData(), size) != size) {
            *message = QObject::tr("Cannot copy APK: %1").arg(copy.errorString());
            return false;
        }
    }
    if (!copy.flush()) {
        *message = QObject::tr("Cannot save APK: %1").arg(copy.errorString());
        return false;
    }
    qInfo("APK library import: stage=inspect");
    const QJsonObject info = report(copy.fileName(), QString(), message);
    if (info.isEmpty()) return false;
    const QString package = info.value(QStringLiteral("package")).toString();
    if (!validPackage(package)) {
        *message = QObject::tr("The APK has an invalid package name.");
        return false;
    }
    if (!info.value(QStringLiteral("runnable")).toBool()) {
        *message = QObject::tr("This APK cannot be started: %1").arg(info.value(QStringLiteral("reason")).toString());
        return false;
    }
    qInfo("APK library import: stage=install");
    const QString lockName = lockPath(package);
    if (lockName.isEmpty()) {
        *message = QObject::tr("Invalid app package.");
        return false;
    }
    QLockFile lock(lockName);
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) {
        *message = QObject::tr("The app is running or its data is in use.");
        return false;
    }
    QDir root(m_root);
    if (!root.mkdir(package)) {
        *message = root.exists(package)
            ? QObject::tr("This app is already installed; updates are not supported yet.")
            : QObject::tr("Cannot create the app directory.");
        return false;
    }
    if (!copy.rename(apkPath(package))) {
        *message = QObject::tr("Cannot install APK: %1").arg(copy.errorString());
        root.rmdir(package);
        return false;
    }
    copy.setAutoRemove(false);
    qInfo("APK library import: result=success");
    if (installedPackage) *installedPackage = package;
    *message = QObject::tr("Added %1 to the library. Compatibility is not guaranteed.").arg(package);
    return true;
}

bool ApkStore::permissions(const QString &package, QVariantList *names, QVariantList *maxSdk, QString *error) const
{
    const QString path = apkPath(package);
    const QString splits = QDir(QFileInfo(path).absolutePath()).filePath(QStringLiteral("splits"));
    const QFileInfo splitsInfo(splits);
    if (path.isEmpty() || !QFileInfo(path).isFile() || splitsInfo.isSymLink() || !names || !maxSdk || !error) {
        if (error) *error = QObject::tr("Cannot inspect the app's permissions.");
        return false;
    }
    const QJsonObject info = report(path, splitsInfo.isDir() ? splits : QString(), error);
    if (info.value(QStringLiteral("package")).toString() != package ||
        !info.value(QStringLiteral("permissions")).isArray() ||
        !info.value(QStringLiteral("permissionMaxSdk")).isArray()) {
        if (error->isEmpty()) *error = QObject::tr("Cannot inspect the app's permissions.");
        return false;
    }
    *names = info.value(QStringLiteral("permissions")).toArray().toVariantList();
    *maxSdk = info.value(QStringLiteral("permissionMaxSdk")).toArray().toVariantList();
    return true;
}

QVariantList ApkStore::apps() const
{
    QVariantList result;
    if (m_root.isEmpty()) return result;
    const QDir root(m_root);
    for (const QFileInfo &dir : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name)) {
        const QString package = dir.fileName();
        const QString path = apkPath(package);
        const QFileInfo apkInfo(path);
        if (path.isEmpty() || !apkInfo.isFile() || apkInfo.isSymLink()) continue;
        QString error;
        const QString splitsPath = QDir(dir.filePath()).filePath(QStringLiteral("splits"));
        const QFileInfo splitsInfo(splitsPath);
        if (splitsInfo.isSymLink()) {
            qWarning("Installed app splits directory is a symbolic link.");
            continue;
        }
        const bool hasSplits = splitsInfo.isDir();
        const QJsonObject info = report(path, hasSplits ? splitsPath : QString(), &error);
        if (info.isEmpty() || info.value(QStringLiteral("package")).toString() != package) {
            qWarning("Cannot inspect installed APK: %s", qPrintable(error));
            continue;
        }
        QVariantMap app;
        app.insert(QStringLiteral("package"), package);
        const QString label = info.value(QStringLiteral("label")).toString();
        app.insert(QStringLiteral("label"), label.isEmpty() ? package : label);
        app.insert(QStringLiteral("versionName"), info.value(QStringLiteral("versionName")).toString());
        app.insert(QStringLiteral("versionCode"), info.value(QStringLiteral("versionCode")).toInt());
        app.insert(QStringLiteral("minSdk"), info.value(QStringLiteral("minSdk")).toInt());
        app.insert(QStringLiteral("targetSdk"), info.value(QStringLiteral("targetSdk")).toInt());
        app.insert(QStringLiteral("permissions"), info.value(QStringLiteral("permissions")).toArray().toVariantList());
        app.insert(QStringLiteral("permissionMaxSdk"), info.value(QStringLiteral("permissionMaxSdk")).toArray().toVariantList());
        app.insert(QStringLiteral("apkBytes"), QFileInfo(path).size());
        quint64 count = 0;
        QString usageError;
        if (!readLaunchCount(QDir(dir.filePath()).filePath(QStringLiteral(".launch-count")), &count, &usageError))
            qWarning("%s", qPrintable(usageError));
        app.insert(QStringLiteral("launchCount"), QVariant::fromValue(count));
        uint8_t *icon = nullptr;
        size_t iconSize = 0;
        const QByteArray apk = path.toUtf8();
        const QByteArray splits = splitsPath.toUtf8();
        const bool hasIcon = hasSplits
            ? megler_apk_icon_splits(apk.constData(), splits.constData(), &icon, &iconSize)
            : megler_apk_icon(apk.constData(), &icon, &iconSize);
        if (hasIcon && icon && iconSize > 0 &&
            iconSize <= static_cast<size_t>(std::numeric_limits<int>::max())) {
            const QByteArray bytes(reinterpret_cast<const char *>(icon), static_cast<int>(iconSize));
            const QByteArray mime = bytes.startsWith("\x89PNG") ? "image/png" : "image/webp";
            app.insert(QStringLiteral("icon"), QString::fromLatin1("data:" + mime + ";base64," + bytes.toBase64()));
        }
        if (icon) megler_free(icon);
        result.append(app);
    }
    std::sort(result.begin(), result.end(), [](const QVariant &left, const QVariant &right) {
        const QVariantMap a = left.toMap();
        const QVariantMap b = right.toMap();
        const int order = QString::localeAwareCompare(a.value(QStringLiteral("label")).toString().toCaseFolded(),
                                                       b.value(QStringLiteral("label")).toString().toCaseFolded());
        return order != 0 ? order < 0 : a.value(QStringLiteral("package")).toString() < b.value(QStringLiteral("package")).toString();
    });
    return result;
}
