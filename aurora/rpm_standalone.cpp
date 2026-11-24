#include <QCoreApplication>
#include "rpm_standalone.h"

#include "apk_store.h"
#include "rpm_icon.h"
#include "rpm_writer.h"
#include "runtime_settings.h"
#include "shared_runtime.h"
#include "permission_settings.h"
#include "standalone_app_mapping.h"
#include "../core/include/megler.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QUuid>

namespace {

bool inside(const QString &path, const QString &directory)
{
    return !directory.isEmpty() &&
        (path == directory || path.startsWith(directory + QLatin1Char('/')));
}

bool addDirectory(const QString &source, const QString &destination,
                  std::vector<RpmWriter::StreamFile> *files, QString *error,
                  const QString &privateData, QSet<QString> *ancestors)
{
    const QFileInfo rootInfo(source);
    const QString resolvedRoot = rootInfo.canonicalFilePath();
    if (resolvedRoot.isEmpty() || !rootInfo.isDir() || !rootInfo.isReadable()) {
        *error = QCoreApplication::translate("RpmStandalone", "Required directory is missing or inaccessible: %1").arg(source);
        return false;
    }
    if (inside(resolvedRoot, privateData)) {
        *error = QCoreApplication::translate("RpmStandalone", "Cannot package private app data: %1").arg(source);
        return false;
    }
    if (ancestors->contains(resolvedRoot)) {
        *error = QCoreApplication::translate("RpmStandalone", "Directory link cycle: %1").arg(source);
        return false;
    }
    ancestors->insert(resolvedRoot);
    const QFileInfoList children = QDir(resolvedRoot).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
    for (const QFileInfo &child : children) {
        const QString target = destination + QLatin1Char('/') + child.fileName();
        if (child.isDir()) {
            if (!addDirectory(child.filePath(), target, files, error, privateData, ancestors)) return false;
        } else if (child.isFile()) {
            const QString resolved = child.canonicalFilePath();
            if (resolved.isEmpty() || inside(resolved, privateData)) {
                *error = QCoreApplication::translate("RpmStandalone", "Cannot package inaccessible file or private app data: %1").arg(child.filePath());
                return false;
            }
            RpmWriter::StreamFile file;
            file.path = target.toUtf8().toStdString();
            file.sourcePath = resolved.toUtf8().toStdString();
            files->push_back(file);
        } else {
            *error = QCoreApplication::translate("RpmStandalone", "Unsupported or inaccessible resource: %1").arg(child.filePath());
            return false;
        }
    }
    ancestors->remove(resolvedRoot);
    return true;
}

void addBytes(std::vector<RpmWriter::StreamFile> *files, const QString &path, const QByteArray &bytes)
{
    RpmWriter::StreamFile file;
    file.path = path.toUtf8().toStdString();
    file.content.assign(bytes.constData(), size_t(bytes.size()));
    files->push_back(file);
}

void hashValue(QCryptographicHash *hash, const QByteArray &value)
{
    hash->addData(QByteArray::number(value.size()));
    hash->addData(":", 1);
    hash->addData(value);
}

QByteArray fileHash(const QString &path)
{
    QFile file(path);
    if (!QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file) || file.error() != QFile::NoError) return {};
    return hash.result();
}

bool hashTree(QCryptographicHash *hash, const QString &path, const QString &relative,
              const QString &privateData, QSet<QString> *ancestors)
{
    const QFileInfo info(path);
    const QString resolved = info.canonicalFilePath();
    if (resolved.isEmpty() || inside(resolved, privateData)) return false;
    hashValue(hash, relative.toUtf8());
    if (info.isFile()) {
        const QByteArray digest = fileHash(resolved);
        if (digest.isEmpty()) return false;
        hashValue(hash, QByteArray("file"));
        hashValue(hash, digest);
        return true;
    }
    if (!info.isDir() || !info.isReadable() || ancestors->contains(resolved)) return false;
    hashValue(hash, QByteArray("directory"));
    ancestors->insert(resolved);
    const QFileInfoList children = QDir(resolved).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
    for (const QFileInfo &child : children)
        if (!hashTree(hash, child.filePath(), relative + QLatin1Char('/') + child.fileName(),
                      privateData, ancestors)) return false;
    ancestors->remove(resolved);
    return true;
}

QString cachePath(const QString &directory)
{
    return QDir(directory).filePath(QStringLiteral(".rpm-cache.json"));
}

}

QByteArray RpmStandalone::cacheFingerprint(const QString &package, const QString &label, const QImage &icon,
                                           const QString &storeRoot, const QString &runtimeRoot,
                                           const QString &qmlRoot, const QString &executable, bool fileDiagnostics,
                                           const QString &managerVersion)
{
    const ApkStore store(storeRoot);
    const QString apk = store.apkPath(package);
    const QFileInfo appDir(QFileInfo(apk).absolutePath());
    const QString abi = SharedRuntime::version(runtimeRoot);
    if (apk.isEmpty() || appDir.isSymLink() || !appDir.isDir() ||
        abi != QLatin1String(STANDALONE_RUNTIME_ABI) || icon.isNull() || managerVersion.isEmpty()) return {};
    QStringList denied;
    QString error;
    if (!PermissionSettings::read(appDir.filePath(), &denied, &error)) return {};
    denied.sort();
    const QString dataPath = store.dataPath(package);
    QString privateData = QFileInfo(dataPath).canonicalFilePath();
    if (privateData.isEmpty())
        privateData = QDir(appDir.canonicalFilePath()).filePath(QStringLiteral("data"));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const QByteArray &value : {QByteArray("standalone-rpm-cache-v3"), managerVersion.toUtf8(),
         package.toUtf8(), label.toUtf8(),
         abi.toUtf8(), RuntimeSettings::selectedLocale().toUtf8(), RuntimeSettings::theme().toUtf8(),
         RuntimeSettings::location().toUtf8(), QJsonDocument(QJsonArray::fromStringList(denied)).toJson(QJsonDocument::Compact),
         QSettings().value(QStringLiteral("googleServices/provider"), QStringLiteral("none")).toString().toUtf8(),
         QByteArray(fileDiagnostics ? "1" : "0")})
        hashValue(&hash, value);
    QByteArray image;
    QBuffer buffer(&image);
    if (!buffer.open(QIODevice::WriteOnly) || !icon.save(&buffer, "PNG")) return {};
    hashValue(&hash, image);
    QSet<QString> ancestors;
    if (!hashTree(&hash, apk, QStringLiteral("apk"), privateData, &ancestors) ||
        !hashTree(&hash, executable, QStringLiteral("launcher"), privateData, &ancestors) ||
        !hashTree(&hash, qmlRoot, QStringLiteral("qml"), privateData, &ancestors)) return {};
    const QString splits = QDir(appDir.filePath()).filePath(QStringLiteral("splits"));
    if (QFileInfo(splits).exists() || QFileInfo(splits).isSymLink()) {
        if (!hashTree(&hash, splits, QStringLiteral("splits"), privateData, &ancestors)) return {};
    } else {
        hashValue(&hash, QByteArray("no-splits"));
    }
    return hash.result().toHex();
}

QString RpmStandalone::cachedRpm(const QString &directory, const QByteArray &fingerprint)
{
    if (fingerprint.size() != 64 || QFileInfo(directory).isSymLink() ||
        !QFileInfo(directory).isDir() || QFileInfo(cachePath(directory)).isSymLink()) return {};
    QFile metadata(cachePath(directory));
    if (!metadata.open(QIODevice::ReadOnly) || metadata.size() > 4096) return {};
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(metadata.readAll(), &error);
    if (metadata.error() != QFile::NoError || error.error != QJsonParseError::NoError || !document.isObject()) return {};
    const QJsonObject object = document.object();
    const QString id = StandaloneAppMapping::read(directory);
    const QString name = object.value(QStringLiteral("file")).toString();
    static const QRegularExpression pattern(QStringLiteral(
        "\\Aru\\.androidcompatibilitylayer\\.Android[0-9a-f]{24}-1\\.0\\.0-1\\.(?:aarch64|armv7hl|x86_64)\\.rpm\\z"));
    if (id.isEmpty() || !pattern.match(name).hasMatch() || !name.startsWith(id + QLatin1Char('-')) ||
        object.value(QStringLiteral("inputs")).toString().toLatin1() != fingerprint) return {};
    const QString path = QDir(directory).filePath(name);
    const QFileInfo rpm(path);
    if (rpm.isSymLink() || !rpm.isFile() ||
        fileHash(path).toHex() != object.value(QStringLiteral("sha256")).toString().toLatin1()) return {};
    return path;
}

bool RpmStandalone::saveCache(const QString &directory, const QString &rpm, const QByteArray &fingerprint)
{
    const QFileInfo dir(directory), file(rpm);
    if (fingerprint.size() != 64 || dir.isSymLink() || !dir.isDir() || file.isSymLink() ||
        !file.isFile() || QDir::cleanPath(file.absolutePath()) != QDir::cleanPath(dir.absoluteFilePath()) ||
        QFileInfo(cachePath(directory)).isSymLink()) return false;
    const QByteArray digest = fileHash(rpm);
    if (digest.isEmpty()) return false;
    QJsonObject object;
    object.insert(QStringLiteral("file"), file.fileName());
    object.insert(QStringLiteral("inputs"), QString::fromLatin1(fingerprint));
    object.insert(QStringLiteral("sha256"), QString::fromLatin1(digest.toHex()));
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    QSaveFile metadata(cachePath(directory));
    return metadata.open(QIODevice::WriteOnly) && metadata.write(bytes) == bytes.size() && metadata.commit();
}

bool RpmStandalone::reuseOrGenerate(const std::function<QString()> &lookup,
                                    const std::function<bool(QString *)> &generate,
                                    QString *output, bool *cacheHit)
{
    *output = lookup();
    *cacheHit = !output->isEmpty();
    return *cacheHit || generate(output);
}

bool RpmStandalone::createUnsigned(const QString &package, const QString &label, const QImage &icon,
                                   const QString &storeRoot, const QString &runtimeRoot,
                                   const QString &qmlRoot, const QString &executable,
                                   const QString &outputDirectory, QString *output, QString *error,
                                   bool fileDiagnostics)
{
    if (output) output->clear();
    if (error) error->clear();
    QString localError;
    QString *problem = error ? error : &localError;
    const ApkStore store(storeRoot);
    const QString apk = store.apkPath(package);
    const QString lockPath = store.lockPath(package);
    if (apk.isEmpty() || lockPath.isEmpty() || !QFileInfo(apk).isFile() ||
        !QFileInfo(executable).isFile() || outputDirectory.isEmpty() || icon.isNull()) {
        *problem = QCoreApplication::translate("RpmStandalone", "Standalone package input is missing or invalid.");
        return false;
    }
    const QString apkFile = QFileInfo(apk).canonicalFilePath();
    const QString binaryFile = QFileInfo(executable).canonicalFilePath();
    const QString dataPath = store.dataPath(package);
    QString privateData = QFileInfo(dataPath).canonicalFilePath();
    if (privateData.isEmpty()) {
        const QString appDirectory = QFileInfo(QFileInfo(dataPath).absolutePath()).canonicalFilePath();
        if (!appDirectory.isEmpty()) privateData = QDir(appDirectory).filePath(QStringLiteral("data"));
    }
    if (apkFile.isEmpty() || binaryFile.isEmpty() ||
        inside(apkFile, privateData) || inside(binaryFile, privateData)) {
        *problem = QCoreApplication::translate("RpmStandalone", "Cannot package inaccessible files or private app data.");
        return false;
    }
    QLockFile lock(lockPath);
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) {
        *problem = QCoreApplication::translate("RpmStandalone", "Close the Android app before creating its RPM.");
        return false;
    }
    QStringList deniedPermissions;
    if (!PermissionSettings::read(QFileInfo(apk).absolutePath(), &deniedPermissions, problem))
        return false;
    QVariantList requestedPermissions, permissionMaxSdk;
    if (!store.permissions(package, &requestedPermissions, &permissionMaxSdk, problem))
        return false;
    const QVariantList permissionGroups = PermissionSettings::requested(requestedPermissions, permissionMaxSdk,
                                                                         deniedPermissions);
    QSet<QString> allowedGroups;
    for (const QVariant &entry : permissionGroups) {
        const QVariantMap group = entry.toMap();
        if (group.value(QStringLiteral("allowed")).toBool())
            allowedGroups.insert(group.value(QStringLiteral("group")).toString());
    }
    bool cameraOwnPictures = false;
    if (allowedGroups.contains(QStringLiteral("camera")) && !allowedGroups.contains(QStringLiteral("storage"))) {
        const QByteArray apkBytes = apk.toUtf8();
        const QFileInfo splitsInfo(QFileInfo(apk).dir().filePath(QStringLiteral("splits")));
        if (splitsInfo.isSymLink()) {
            *problem = QCoreApplication::translate("RpmStandalone", "Could not inspect camera picture-storage permissions.");
            return false;
        }
        const QByteArray splitBytes = splitsInfo.filePath().toUtf8();
        char *raw = splitsInfo.isDir() ? megler_apk_report_splits(apkBytes.constData(), splitBytes.constData())
                                      : megler_apk_report(apkBytes.constData());
        if (!raw) {
            *problem = QCoreApplication::translate("RpmStandalone", "Could not inspect camera picture-storage permissions.");
            return false;
        }
        QJsonParseError parseError;
        const QJsonDocument report = QJsonDocument::fromJson(QByteArray(raw), &parseError);
        megler_free(raw);
        const QJsonObject cameraInfo = report.object();
        const QJsonValue targetSdk = cameraInfo.value(QStringLiteral("targetSdk"));
        if (parseError.error != QJsonParseError::NoError || !report.isObject() ||
            !cameraInfo.value(QStringLiteral("ok")).toBool() ||
            cameraInfo.value(QStringLiteral("package")).toString() != package ||
            !targetSdk.isDouble() || targetSdk.toInt(-1) < 0 ||
            targetSdk.toDouble() != targetSdk.toInt(-1)) {
            *problem = QCoreApplication::translate("RpmStandalone", "Invalid camera picture-storage metadata.");
            return false;
        }
        int effectiveSdk = targetSdk.toInt();
        if (effectiveSdk == 0) {
            const QJsonValue minSdk = cameraInfo.value(QStringLiteral("minSdk"));
            if (!minSdk.isDouble() || minSdk.toInt(-1) < 0 || minSdk.toDouble() != minSdk.toInt(-1)) {
                *problem = QCoreApplication::translate("RpmStandalone", "Invalid camera picture-storage metadata.");
                return false;
            }
            effectiveSdk = minSdk.toInt() > 0 ? minSdk.toInt() : 1;
        }
        cameraOwnPictures = effectiveSdk >= 29;
    }
    QString random = QUuid::createUuid().toString();
    random.remove(QLatin1Char('{'));
    random.remove(QLatin1Char('}'));
    random.remove(QLatin1Char('-'));
    const QString organization = QStringLiteral("ru.androidcompatibilitylayer");
    const QString appId = organization + QStringLiteral(".Android") + random.left(24);
    const QString dataRoot = QStringLiteral("/usr/share/") + appId;
    const QString binaryPath = QStringLiteral("/usr/bin/") + appId;
    std::vector<RpmWriter::StreamFile> files;
    RpmWriter::StreamFile binary;
    binary.path = binaryPath.toStdString();
    binary.sourcePath = binaryFile.toUtf8().toStdString();
    binary.mode = 0755;
    files.push_back(binary);
    RpmWriter::StreamFile base;
    base.path = (dataRoot + QStringLiteral("/apk/base.apk")).toStdString();
    base.sourcePath = apkFile.toUtf8().toStdString();
    files.push_back(base);
    QSet<QString> ancestors;
    const QString splitRoot = QDir(QFileInfo(apk).absolutePath()).filePath(QStringLiteral("splits"));
    if ((QFileInfo(splitRoot).exists() || QFileInfo(splitRoot).isSymLink()) &&
        !addDirectory(splitRoot, dataRoot + QStringLiteral("/apk/splits"), &files, problem, privateData, &ancestors))
        return false;
    const QString runtimeVersion = SharedRuntime::version(runtimeRoot);
    if (runtimeVersion.isEmpty()) {
        *problem = QCoreApplication::translate("RpmStandalone", "The shared Android runtime is unavailable.");
        return false;
    }
    if (runtimeVersion != QLatin1String(STANDALONE_RUNTIME_ABI)) {
        *problem = QCoreApplication::translate("RpmStandalone",
            "The manager and shared Android runtime are incompatible. Restart the manager after updating it.");
        return false;
    }
    if (!addDirectory(qmlRoot, dataRoot + QStringLiteral("/qml"), &files, problem, privateData, &ancestors))
        return false;
    QString title = label.trimmed();
    title.replace(QRegularExpression(QStringLiteral("[\\r\\n\\t]+")), QStringLiteral(" "));
    if (title.isEmpty()) title = package;
    title = title.left(512);
    const bool sharedMicrog = package != QLatin1String("com.google.android.gms") &&
        QSettings().value(QStringLiteral("googleServices/provider"), QStringLiteral("none")).toString() ==
            QLatin1String("microg");
    QJsonObject metadata;
    metadata.insert(QStringLiteral("appId"), appId);
    metadata.insert(QStringLiteral("sharedRuntimeAbi"), runtimeVersion);
    metadata.insert(QStringLiteral("standaloneSnapshots"), true);
    metadata.insert(QStringLiteral("fileDiagnostics"), fileDiagnostics);
    metadata.insert(QStringLiteral("sharedMicrog"), sharedMicrog);
    metadata.insert(QStringLiteral("package"), package);
    metadata.insert(QStringLiteral("displayName"), title);
    metadata.insert(QStringLiteral("locale"), RuntimeSettings::selectedLocale());
    metadata.insert(QStringLiteral("theme"), RuntimeSettings::theme());
    metadata.insert(QStringLiteral("location"), RuntimeSettings::location());
    metadata.insert(QStringLiteral("deniedPermissions"), QJsonArray::fromStringList(deniedPermissions));
    addBytes(&files, dataRoot + QStringLiteral("/standalone.json"),
             QJsonDocument(metadata).toJson(QJsonDocument::Compact));
    const auto requests = [&](const char *name) {
        const QString permission = QStringLiteral("android.permission.") + QLatin1String(name);
        for (int i = 0; i < requestedPermissions.size(); ++i) {
            const int max = i < permissionMaxSdk.size() ? permissionMaxSdk.at(i).toInt() : 0;
            if ((max == 0 || max >= 34) && requestedPermissions.at(i).toString() == permission)
                return true;
        }
        return false;
    };
    QStringList osPermissions;
    if (requests("INTERNET")) osPermissions << QStringLiteral("Internet");
    osPermissions << QStringLiteral("Audio");
    osPermissions << QStringLiteral("ru.androidcompatibilitylayer.appmanager.log@ru.androidcompatibilitylayer.appmanager");
    osPermissions << QStringLiteral("ru.androidcompatibilitylayer.appmanager@ru.androidcompatibilitylayer.appmanager");
    if (sharedMicrog) {
        osPermissions << QStringLiteral("ru.androidcompatibilitylayer.appmanager.microg@ru.androidcompatibilitylayer.appmanager");
        osPermissions << QStringLiteral("ru.omp.am.Identify@ru.omp.am");
    }
    if (allowedGroups.contains(QStringLiteral("camera"))) osPermissions << QStringLiteral("Camera");
    if (allowedGroups.contains(QStringLiteral("nearby_devices")) &&
        (requests("BLUETOOTH") || requests("BLUETOOTH_ADMIN") || requests("BLUETOOTH_SCAN") ||
         requests("BLUETOOTH_CONNECT") || requests("BLUETOOTH_ADVERTISE")))
        osPermissions << QStringLiteral("Bluetooth");
    if (allowedGroups.contains(QStringLiteral("location"))) osPermissions << QStringLiteral("Location");
    if (allowedGroups.contains(QStringLiteral("microphone"))) osPermissions << QStringLiteral("Microphone");
    if (allowedGroups.contains(QStringLiteral("phone")) &&
        (requests("CALL_PHONE") || requests("ANSWER_PHONE_CALLS"))) osPermissions << QStringLiteral("Call");
    if (allowedGroups.contains(QStringLiteral("sensors")) ||
        allowedGroups.contains(QStringLiteral("activity_recognition"))) osPermissions << QStringLiteral("Sensors");
    if (allowedGroups.contains(QStringLiteral("storage"))) {
        osPermissions << QStringLiteral("UserDirs");
    } else {
        if (allowedGroups.contains(QStringLiteral("read_media_visual"))) {
            if (requests("READ_MEDIA_IMAGES") || requests("READ_MEDIA_VISUAL_USER_SELECTED"))
                osPermissions << QStringLiteral("Pictures");
            if (requests("READ_MEDIA_VIDEO") || requests("READ_MEDIA_VISUAL_USER_SELECTED"))
                osPermissions << QStringLiteral("Videos");
        }
        if (cameraOwnPictures && !osPermissions.contains(QStringLiteral("Pictures")))
            osPermissions << QStringLiteral("Pictures");
        if (allowedGroups.contains(QStringLiteral("read_media_aural"))) osPermissions << QStringLiteral("Music");
    }
    const QString desktop = QStringLiteral(
        "[Desktop Entry]\nType=Application\nX-Nemo-Application-Type=silica-qt5\n"
        "Icon=%1\nExec=%2 --standalone\nName=%3\n\n[X-Application]\n"
        "Permissions=%4\n"
        "Intents=Start\n"
        "OrganizationName=%5\nApplicationName=%6\n")
        .arg(appId, binaryPath, title, osPermissions.join(QLatin1Char(';')),
             organization, appId.mid(organization.size() + 1));
    addBytes(&files, QStringLiteral("/usr/share/applications/") + appId + QStringLiteral(".desktop"),
             desktop.toUtf8());
    const int sizes[] = {86, 108, 128, 172};
    for (int size : sizes) {
        const QByteArray png = RpmIcon::png(icon, size);
        if (png.isEmpty()) {
            *problem = QCoreApplication::translate("RpmStandalone", "Cannot create the application icon.");
            return false;
        }
        addBytes(&files, QStringLiteral("/usr/share/icons/hicolor/%1x%1/apps/").arg(size)
                 + appId + QStringLiteral(".png"), png);
    }
    if (!QDir().mkpath(outputDirectory)) {
        *problem = QCoreApplication::translate("RpmStandalone", "Cannot create the RPM output directory.");
        return false;
    }
    RpmWriter::Package rpm;
    rpm.name = appId.toStdString();
    rpm.version = "1.0.0";
    rpm.release = "1";
    rpm.summary = "Standalone Android application";
    rpm.description = "Android application using the manager's shared compatibility runtime.";
    rpm.license = "GPL-3.0-only AND LicenseRef-AndroidApp";
    rpm.buildTime = uint32_t(QDateTime::currentMSecsSinceEpoch() / 1000);
#if defined(Q_PROCESSOR_ARM_64)
    rpm.arch = "aarch64";
#elif defined(__arm__) && defined(__ARM_PCS_VFP)
    rpm.arch = "armv7hl";
#elif defined(Q_PROCESSOR_X86_64)
    rpm.arch = "x86_64";
#else
    *problem = QCoreApplication::translate("RpmStandalone", "Unsupported executable architecture.");
    return false;
#endif
    const char *libraries[] = {
        "libEGL.so.1", "libGLESv2.so.2", "libQt5Core.so.5",
        "libQt5DBus.so.5", "libQt5Gui.so.5", "libQt5Multimedia.so.5",
        "libQt5Network.so.5", "libQt5Positioning.so.5", "libQt5Qml.so.5", "libQt5Quick.so.5",
        "libQt5Sensors.so.5", "libauroraapp.so.2", "libc.so.6",
        "libgcc_s.so.1", "libm.so.6", "libruntime-manager-qt5.so.1",
        "libstdc++.so.6", "nemo-qml-plugin-notifications-qt5", "qt5-plugin-imageformat-jpeg"
    };
    for (const char *name : libraries) {
        RpmWriter::Package::Requirement requirement;
        requirement.name = name;
        if (sizeof(void *) == 8 && requirement.name.find(".so.") != std::string::npos)
            requirement.name += "()(64bit)";
        rpm.requires.push_back(requirement);
    }
    RpmWriter::Package::Requirement silica;
    silica.name = "sailfishsilica-qt5";
    silica.version = "0.10.9";
    silica.flags = 12; // RPMSENSE_GREATER | RPMSENSE_EQUAL
    rpm.requires.push_back(silica);
    const QString path = QDir(outputDirectory).filePath(appId + QStringLiteral("-1.0.0-1.")
                                                       + QString::fromStdString(rpm.arch) + QStringLiteral(".rpm"));
    std::string writerError;
    if (!RpmWriter::writeStreaming(rpm, files, path.toUtf8().toStdString(), &writerError)) {
        *problem = QString::fromStdString(writerError);
        return false;
    }
    if (output) *output = path;
    return true;
}
