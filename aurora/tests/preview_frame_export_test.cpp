#include "../preview_frame_export.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cstdio>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

int main()
{
    QTemporaryDir temporary;
    CHECK(temporary.isValid());
    const QString documents = temporary.path();
    const QDateTime now = QDateTime::fromString(QStringLiteral("2026-11-22T12:00:00Z"), Qt::ISODate);
    QImage glFrame(4, 3, QImage::Format_ARGB32);
    glFrame.fill(qRgba(255, 0, 0, 255));
    glFrame.setText(QStringLiteral("location"), QStringLiteral("must not be exported"));
    QImage composed(3, 4, QImage::Format_ARGB32);
    composed.fill(qRgba(0, 0, 255, 255));

    QString firstPath, error;
    CHECK(PreviewFrameExport::savePair(documents, 17, now, glFrame, composed, now, &firstPath, &error));
    CHECK(error.isEmpty());
    CHECK(QFileInfo(firstPath).isDir());
    CHECK(QFileInfo(firstPath).canonicalPath() == QFileInfo(QDir(documents).filePath(
          QStringLiteral("AndroidCompatibilityLayer-Frames"))).canonicalFilePath());
    const QImage savedGl(QDir(firstPath).filePath(QStringLiteral("gl-before-transfer.png")));
    const QImage savedComposed(QDir(firstPath).filePath(QStringLiteral("composed-after.png")));
    CHECK(savedGl.size() == glFrame.size());
    CHECK(savedComposed.size() == composed.size());
    CHECK(savedGl.pixel(0, 0) == glFrame.pixel(0, 0));
    CHECK(savedComposed.pixel(0, 0) == composed.pixel(0, 0));
    CHECK(savedGl.textKeys().isEmpty());
    QFile manifestFile(QDir(firstPath).filePath(QStringLiteral("manifest.json")));
    CHECK(manifestFile.open(QIODevice::ReadOnly));
    const QJsonObject manifest = QJsonDocument::fromJson(manifestFile.readAll()).object();
    CHECK(manifest.value(QStringLiteral("frame_sequence")).toInt() == 17);
    CHECK(manifest.value(QStringLiteral("gl_before_transfer")).toObject()
              .value(QStringLiteral("width")).toInt() == glFrame.width());
    CHECK(manifest.value(QStringLiteral("composed_after")).toObject()
              .value(QStringLiteral("height")).toInt() == composed.height());
    CHECK(manifest.value(QStringLiteral("expires_after_utc")).toString() ==
          now.addDays(PreviewFrameExport::RetentionDays).toString(Qt::ISODate));
    CHECK(QFileInfo(firstPath).permissions().testFlag(QFile::ExeOwner));

    QString secondPath, thirdPath;
    CHECK(PreviewFrameExport::savePair(documents, 18, now, glFrame, composed, now, &secondPath, &error));
    CHECK(PreviewFrameExport::savePair(documents, 19, now, glFrame, composed, now, &thirdPath, &error));
    QString rejectedPath;
    CHECK(!PreviewFrameExport::savePair(documents, 20, now, glFrame, composed, now, &rejectedPath, &error));
    CHECK(error.contains(QStringLiteral("Three frame-pair exports")));
    CHECK(rejectedPath.isEmpty());

    QImage oversized(5000, 1, QImage::Format_ARGB32);
    CHECK(!PreviewFrameExport::savePair(documents, 21, now, oversized, composed, now, &rejectedPath, &error));
    CHECK(error.contains(QStringLiteral("exceeds the supported size")));
    CHECK(!PreviewFrameExport::savePair(documents, 0, now, glFrame, composed, now, &rejectedPath, &error));
    CHECK(error.contains(QStringLiteral("sequence")));

    const QDateTime afterRetention = now.addDays(PreviewFrameExport::RetentionDays + 1);
    CHECK(PreviewFrameExport::cleanupExpiredPairs(documents, afterRetention, &error));
    CHECK(!QFileInfo::exists(firstPath));
    CHECK(!QFileInfo::exists(secondPath));
    CHECK(!QFileInfo::exists(thirdPath));
    QString retainedPath;
    CHECK(PreviewFrameExport::savePair(documents, 22, afterRetention, glFrame, composed,
                                       afterRetention, &retainedPath, &error));
    CHECK(QFileInfo(retainedPath).isDir());

    std::puts("Qt preview frame export OK");
    return 0;
}
