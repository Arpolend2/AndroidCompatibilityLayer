#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_ERROR_LOG_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_ERROR_LOG_H

#include <QString>

namespace ErrorLog {
bool initialize(const QString &application, QString *error, const QString &sharedRoot = QString());
void setApplicationName(const QString &application);
void record(const QString &source, const QString &message, bool error);
QString displayText();
QString report();
bool saveReport(QString *path, QString *error);
bool detailedEnabled(bool *enabled, QString *error);
bool detailedCaptureEnabled();
bool setDetailedEnabled(bool enabled, QString *error);
bool detailedReport(QString *text, QString *error);
bool saveDetailedReport(QString *path, QString *error);
bool startCoreChannel(QString *error);
void sendCoreLine(int level, const char *line);
bool startCollector(QString *error);
bool startForwarding(QString *error);
bool startPersistence(const QString &privateRoot, QString *error);
QString persistenceError();
bool startStandaloneDiagnosticFile(QString *path, QString *error);
QString standaloneDiagnosticFileError();
}

#endif
