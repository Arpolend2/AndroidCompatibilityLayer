#include "preview_frame_export.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QUuid>

#include <limits>

namespace {
const char ExportDirectoryName[] = "AndroidCompatibilityLayer-Frames";
const qint64 MaximumFrameBytes = 32 * 1024 * 1024;
const int MaximumFrameDimension = 4096;
const QFile::Permissions OwnerDirectoryPermissions =
    QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
const QFile::Permissions OwnerFilePermissions = QFile::ReadOwner | QFile::WriteOwner;

QString pairPattern()
{
    return QStringLiteral("\\AFramePair-([0-9]{8}-[0-9]{6})-([0-9a-f]{32})\\z");
}

bool canonicalDirectory(const QString &path, bool rejectSymlink, QString *canonical, QString *error)
{
    const QFileInfo info(path);
    if (!info.isDir() || (rejectSymlink && info.isSymLink())) {
        if (error) *error = QStringLiteral("A required directory is missing or is a symbolic link.");
        return false;
    }
    const QString resolved = info.canonicalFilePath();
    if (resolved.isEmpty()) {
        if (error) *error = QStringLiteral("A required directory cannot be resolved.");
        return false;
    }
    if (canonical) *canonical = QDir::cleanPath(resolved);
    return true;
}

bool openPrivateFile(QSaveFile *file, QString *error)
{
    file->setDirectWriteFallback(false);
    if (!file->open(QIODevice::WriteOnly)) {
        if (error) *error = file->errorString();
        return false;
    }
    if (!file->setPermissions(OwnerFilePermissions)) {
        if (error) *error = QStringLiteral("Could not restrict access to a diagnostic file.");
        return false;
    }
    return true;
}

bool writeFile(const QString &path, const QByteArray &contents, QString *error)
{
    QSaveFile file(path);
    if (!openPrivateFile(&file, error))
        return false;
    if (file.write(contents) != contents.size()) {
        if (error) *error = file.errorString();
        return false;
    }
    if (!file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

bool validFrame(const QImage &image, QString *error)
{
    const qint64 frameBytes = static_cast<qint64>(image.bytesPerLine()) * image.height();
    if (image.isNull() || image.width() <= 0 || image.height() <= 0 ||
        image.width() > MaximumFrameDimension || image.height() > MaximumFrameDimension ||
        frameBytes <= 0 || frameBytes > MaximumFrameBytes) {
        if (error) *error = QStringLiteral("A frame is empty or exceeds the supported size.");
        return false;
    }
    return true;
}

bool writeImage(const QString &path, const QImage &image, QString *error)
{
    if (!validFrame(image, error))
        return false;
    QImage pixels(image.size(), QImage::Format_RGBA8888);
    if (pixels.isNull()) {
        if (error) *error = QStringLiteral("Could not allocate a metadata-free frame copy.");
        return false;
    }
    pixels.fill(Qt::transparent);
    {
        QPainter painter(&pixels);
        if (!painter.isActive()) {
            if (error) *error = QStringLiteral("Could not initialize the diagnostic frame copy.");
            return false;
        }
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.drawImage(QPoint(0, 0), image);
        if (!painter.end()) {
            if (error) *error = QStringLiteral("Could not copy diagnostic frame pixels.");
            return false;
        }
    }

    QSaveFile file(path);
    if (!openPrivateFile(&file, error))
        return false;
    if (!pixels.save(&file, "PNG")) {
        if (error) *error = QStringLiteral("Could not encode a diagnostic frame as PNG.");
        return false;
    }
    if (!file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

bool bundleTime(const QString &name, QDateTime *time)
{
    static const QRegularExpression pattern(pairPattern());
    const QRegularExpressionMatch match = pattern.match(name);
    if (!match.hasMatch()) return false;
    const QDateTime parsed = QDateTime::fromString(match.captured(1), QStringLiteral("yyyyMMdd-HHmmss"));
    if (!parsed.isValid()) return false;
    if (time) *time = QDateTime(parsed.date(), parsed.time(), Qt::UTC);
    return true;
}

bool removeBundle(const QString &root, const QFileInfo &entry, QString *error)
{
    if (entry.isSymLink() || !entry.isDir() || entry.canonicalPath() != root) {
        if (error) *error = QStringLiteral("Refusing to remove an unexpected diagnostic export path.");
        return false;
    }
    QDir bundle(entry.absoluteFilePath());
    const QStringList expected = QStringList()
        << QStringLiteral("gl-before-transfer.png")
        << QStringLiteral("composed-after.png")
        << QStringLiteral("manifest.json");
    const QStringList contents = bundle.entryList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : contents) {
        const QFileInfo child(bundle.filePath(name));
        if (!expected.contains(name) || child.isSymLink() || !child.isFile()) {
            if (error) *error = QStringLiteral("Refusing to remove a diagnostic folder with unexpected contents.");
            return false;
        }
    }
    for (const QString &name : contents) {
        if (!QFile::remove(bundle.filePath(name))) {
            if (error) *error = QStringLiteral("Could not remove an expired diagnostic frame.");
            return false;
        }
    }
    if (!QDir(root).rmdir(entry.fileName())) {
        if (error) *error = QStringLiteral("Could not remove an expired diagnostic folder.");
        return false;
    }
    return true;
}

bool purgeExpired(const QString &root, const QDateTime &now, QString *error)
{
    const QDir directory(root);
    const QFileInfoList entries = directory.entryInfoList(
        QStringList() << QStringLiteral("FramePair-*"),
        QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &entry : entries) {
        QDateTime created;
        if (!bundleTime(entry.fileName(), &created)) continue;
        if (created.addDays(PreviewFrameExport::RetentionDays) <= now &&
            !removeBundle(root, entry, error))
            return false;
    }
    return true;
}

bool countBundles(const QString &root, int *count, QString *error)
{
    *count = 0;
    const QDir directory(root);
    const QFileInfoList entries = directory.entryInfoList(
        QStringList() << QStringLiteral("FramePair-*"),
        QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (!bundleTime(entry.fileName(), nullptr)) continue;
        if (entry.isSymLink() || !entry.isDir() || entry.canonicalPath() != root) {
            if (error) *error = QStringLiteral("A diagnostic export folder cannot be verified.");
            return false;
        }
        ++*count;
    }
    return true;
}
}

bool PreviewFrameExport::cleanupExpiredPairs(const QString &documentsDirectory,
                                             const QDateTime &now, QString *error)
{
    if (error) error->clear();
    if (!now.isValid()) {
        if (error) *error = QStringLiteral("The cleanup time is invalid.");
        return false;
    }
    if (documentsDirectory.isEmpty() || !QFileInfo(documentsDirectory).isDir())
        return true;

    QString documentsPath;
    if (!canonicalDirectory(documentsDirectory, false, &documentsPath, error))
        return false;
    const QString exportPath = QDir(documentsPath).filePath(QLatin1String(ExportDirectoryName));
    if (!QFileInfo::exists(exportPath))
        return true;

    QString canonicalExportPath;
    if (!canonicalDirectory(exportPath, true, &canonicalExportPath, error) ||
        QFileInfo(exportPath).canonicalPath() != documentsPath) {
        if (error && error->isEmpty())
            *error = QStringLiteral("The diagnostic export folder is outside Documents.");
        return false;
    }
    if (!QFile::setPermissions(canonicalExportPath, OwnerDirectoryPermissions)) {
        if (error) *error = QStringLiteral("Could not restrict access to the diagnostic export folder.");
        return false;
    }
    return purgeExpired(canonicalExportPath, now.toUTC(), error);
}

bool PreviewFrameExport::savePair(const QString &documentsDirectory, quint64 frameSequence,
                                  const QDateTime &capturedAt, const QImage &glBeforeTransfer,
                                  const QImage &composedAfter, const QDateTime &now,
                                  QString *savedDirectory, QString *error)
{
    if (savedDirectory) savedDirectory->clear();
    if (error) error->clear();
    if (frameSequence == 0 || frameSequence > static_cast<quint64>(std::numeric_limits<qint64>::max()) ||
        !capturedAt.isValid() || !now.isValid()) {
        if (error) *error = QStringLiteral("The frame pair is missing its sequence or timestamp.");
        return false;
    }
    if (!validFrame(glBeforeTransfer, error) || !validFrame(composedAfter, error))
        return false;

    QString documentsPath;
    if (!canonicalDirectory(documentsDirectory, false, &documentsPath, error))
        return false;

    const QString exportPath = QDir(documentsPath).filePath(QLatin1String(ExportDirectoryName));
    if (!QFileInfo::exists(exportPath)) {
        QTemporaryDir rootStaging(QDir(documentsPath).filePath(QStringLiteral(".frame-export-XXXXXX")));
        if (!rootStaging.isValid() || !QFile::setPermissions(rootStaging.path(), OwnerDirectoryPermissions)) {
            if (error) *error = QStringLiteral("Could not create a private diagnostic export folder in Documents.");
            return false;
        }
        const QString stagingPath = rootStaging.path();
        if (!QDir().rename(stagingPath, exportPath)) {
            if (!QFileInfo(exportPath).isDir()) {
                if (error) *error = QStringLiteral("Could not create the diagnostic export folder in Documents.");
                return false;
            }
        }
    }
    QString canonicalExportPath;
    if (!canonicalDirectory(exportPath, true, &canonicalExportPath, error) ||
        QFileInfo(exportPath).canonicalPath() != documentsPath) {
        if (error && error->isEmpty())
            *error = QStringLiteral("The diagnostic export folder is outside Documents.");
        return false;
    }
    if (!QFile::setPermissions(canonicalExportPath, OwnerDirectoryPermissions)) {
        if (error) *error = QStringLiteral("Could not restrict access to the diagnostic export folder.");
        return false;
    }
    if (!purgeExpired(canonicalExportPath, now.toUTC(), error))
        return false;

    int savedPairs = 0;
    if (!countBundles(canonicalExportPath, &savedPairs, error))
        return false;
    if (savedPairs >= MaximumSavedPairs) {
        if (error) *error = QStringLiteral("Three frame-pair exports are already retained. Remove an older folder from Documents first.");
        return false;
    }

    QTemporaryDir staging(QDir(canonicalExportPath).filePath(QStringLiteral(".framepair-XXXXXX")));
    if (!staging.isValid() || !QFile::setPermissions(staging.path(), OwnerDirectoryPermissions)) {
        if (error) *error = QStringLiteral("Could not create a private staging folder for the frame pair.");
        return false;
    }

    const QDateTime capturedUtc = capturedAt.toUTC();
    const QDateTime expiresUtc = now.toUTC().addDays(RetentionDays);
    QJsonObject glStage;
    glStage.insert(QStringLiteral("file"), QStringLiteral("gl-before-transfer.png"));
    glStage.insert(QStringLiteral("width"), glBeforeTransfer.width());
    glStage.insert(QStringLiteral("height"), glBeforeTransfer.height());
    QJsonObject composedStage;
    composedStage.insert(QStringLiteral("file"), QStringLiteral("composed-after.png"));
    composedStage.insert(QStringLiteral("width"), composedAfter.width());
    composedStage.insert(QStringLiteral("height"), composedAfter.height());
    QJsonObject manifest;
    manifest.insert(QStringLiteral("format"), QStringLiteral("android-compatibility-layer-frame-pair"));
    manifest.insert(QStringLiteral("version"), 1);
    manifest.insert(QStringLiteral("frame_sequence"), static_cast<qint64>(frameSequence));
    manifest.insert(QStringLiteral("captured_at_utc"), capturedUtc.toString(Qt::ISODate));
    manifest.insert(QStringLiteral("expires_after_utc"), expiresUtc.toString(Qt::ISODate));
    manifest.insert(QStringLiteral("gl_before_transfer"), glStage);
    manifest.insert(QStringLiteral("composed_after"), composedStage);

    QString writeError;
    if (!writeImage(QDir(staging.path()).filePath(QStringLiteral("gl-before-transfer.png")),
                    glBeforeTransfer, &writeError) ||
        !writeImage(QDir(staging.path()).filePath(QStringLiteral("composed-after.png")),
                    composedAfter, &writeError) ||
        !writeFile(QDir(staging.path()).filePath(QStringLiteral("manifest.json")),
                   QJsonDocument(manifest).toJson(QJsonDocument::Indented), &writeError)) {
        if (error) *error = writeError;
        return false;
    }

    QString unique = QUuid::createUuid().toString();
    unique.remove(QLatin1Char('{'));
    unique.remove(QLatin1Char('}'));
    unique.remove(QLatin1Char('-'));
    const QString bundleName = QStringLiteral("FramePair-%1-%2")
        .arg(now.toUTC().toString(QStringLiteral("yyyyMMdd-HHmmss")), unique);
    const QString bundlePath = QDir(canonicalExportPath).filePath(bundleName);
    const QString stagingPath = staging.path();
    if (!QDir().rename(stagingPath, bundlePath)) {
        if (error) *error = QStringLiteral("Could not publish the complete frame-pair folder.");
        return false;
    }
    if (savedDirectory) *savedDirectory = bundlePath;
    return true;
}
