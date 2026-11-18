#include <QCoreApplication>
#include "rpm_standalone.h"

#include "apk_store.h"
#include "rpm_icon.h"
#include "rpm_writer.h"
#include "runtime_settings.h"
#include "permission_settings.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSet>
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

}

bool RpmStandalone::createUnsigned(const QString &package, const QString &label, const QImage &icon,
                                   const QString &storeRoot, const QString &runtimeRoot,
                                   const QString &qmlRoot, const QString &executable,
                                   const QString &outputDirectory, QString *output, QString *error)
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
    QString random = QUuid::createUuid().toString();
    random.remove(QLatin1Char('{'));
    random.remove(QLatin1Char('}'));
    random.remove(QLatin1Char('-'));
    const QString appId = QStringLiteral("ru.megler.Android") + random.left(24);
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
    if (!addDirectory(runtimeRoot, dataRoot + QStringLiteral("/android-runtime"), &files, problem, privateData, &ancestors) ||
        !addDirectory(qmlRoot, dataRoot + QStringLiteral("/qml"), &files, problem, privateData, &ancestors))
        return false;
    QString title = label.trimmed();
    title.replace(QRegularExpression(QStringLiteral("[\\r\\n\\t]+")), QStringLiteral(" "));
    if (title.isEmpty()) title = package;
    title = title.left(512);
    QJsonObject metadata;
    metadata.insert(QStringLiteral("appId"), appId);
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
        if (allowedGroups.contains(QStringLiteral("read_media_aural"))) osPermissions << QStringLiteral("Music");
    }
    const QString desktop = QStringLiteral(
        "[Desktop Entry]\nType=Application\nX-Nemo-Application-Type=silica-qt5\n"
        "Icon=%1\nExec=%2 --standalone\nName=%3\n\n[X-Application]\n"
        "Permissions=%4\n"
        "OrganizationName=ru.megler\nApplicationName=%5\n")
        .arg(appId, binaryPath, title, osPermissions.join(QLatin1Char(';')), appId.mid(10));
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
    rpm.description = "Android application with its own compatibility runtime.";
    rpm.license = "GPL-3.0-only AND LicenseRef-AndroidApp";
    rpm.buildTime = uint32_t(QDateTime::currentMSecsSinceEpoch() / 1000);
#if defined(Q_PROCESSOR_ARM_64)
    rpm.arch = "aarch64";
#elif defined(Q_PROCESSOR_X86_64)
    rpm.arch = "x86_64";
#else
    *problem = QCoreApplication::translate("RpmStandalone", "Unsupported executable architecture.");
    return false;
#endif
    const char *libraries[] = {
        "libEGL.so.1()(64bit)", "libGLESv2.so.2()(64bit)", "libQt5Core.so.5()(64bit)",
        "libQt5DBus.so.5()(64bit)", "libQt5Gui.so.5()(64bit)", "libQt5Multimedia.so.5()(64bit)",
        "libQt5Network.so.5()(64bit)", "libQt5Positioning.so.5()(64bit)", "libQt5Qml.so.5()(64bit)", "libQt5Quick.so.5()(64bit)",
        "libQt5Sensors.so.5()(64bit)", "libauroraapp.so.2()(64bit)", "libc.so.6()(64bit)",
        "libgcc_s.so.1()(64bit)", "libm.so.6()(64bit)", "libruntime-manager-qt5.so.1()(64bit)",
        "libstdc++.so.6()(64bit)", "nemo-qml-plugin-notifications-qt5", "qt5-plugin-imageformat-jpeg"
    };
    for (const char *name : libraries) {
        RpmWriter::Package::Requirement requirement;
        requirement.name = name;
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
