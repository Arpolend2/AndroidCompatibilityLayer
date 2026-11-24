#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_PREVIEW_FRAME_EXPORT_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_PREVIEW_FRAME_EXPORT_H

#include <QDateTime>
#include <QImage>
#include <QString>

#include <QtGlobal>

namespace PreviewFrameExport {

const int MaximumSavedPairs = 3;
const int RetentionDays = 7;

bool cleanupExpiredPairs(const QString &documentsDirectory, const QDateTime &now, QString *error);
bool savePair(const QString &documentsDirectory, quint64 frameSequence,
              const QDateTime &capturedAt, const QImage &glBeforeTransfer,
              const QImage &composedAfter, const QDateTime &now,
              QString *savedDirectory, QString *error);

}

#endif
