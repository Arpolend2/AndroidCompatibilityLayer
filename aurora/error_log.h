#ifndef MEGLER_AURORA_ERROR_LOG_H
#define MEGLER_AURORA_ERROR_LOG_H

#include <QString>

namespace ErrorLog {
bool initialize(const QString &application, QString *error);
void setApplicationName(const QString &application);
void record(const QString &source, const QString &message, bool error);
bool saveText(const QString &text, QString *path, QString *error);
bool startCoreChannel(QString *error);
void sendCoreLine(int level, const char *line);
}

#endif
