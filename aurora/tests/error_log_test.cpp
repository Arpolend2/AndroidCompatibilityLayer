#include "../error_log.h"
#include "../diagnostic_log.h"
#include "../../core/include/megler.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cstdio>

int main(int argc, char **argv)
{
    QTemporaryDir home;
    if (!home.isValid()) return 1;
    qputenv("HOME", QFile::encodeName(home.path()));
    QCoreApplication app(argc, argv);
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (!QDir().mkpath(documents)) return 1;
    QString error;
    QString savedPath;
    if (ErrorLog::saveText(QStringLiteral("not saved"), &savedPath, &error) ||
        error.isEmpty() || !savedPath.isEmpty()) return 1;
    if (!ErrorLog::initialize(QStringLiteral("Manager"), &error)) {
        std::fprintf(stderr, "Initialization failed: %s\n", qPrintable(error));
        return 1;
    }
    const QDir folder(QDir(documents).filePath(QStringLiteral("Megler-Error-Logs")));
    ErrorLog::record(QStringLiteral("test"), QStringLiteral("previous context"), false);
    if (!folder.entryList(QStringList() << "*.txt", QDir::Files).isEmpty()) return 1;
    ErrorLog::setApplicationName(QString::fromUtf8("Моё приложение/unsafe"));
    ErrorLog::record(QStringLiteral("test"), QStringLiteral("failure"), true);
    QStringList files = folder.entryList(QStringList() << "*.txt", QDir::Files);
    if (files.size() != 1 || !files.first().startsWith(QString::fromUtf8("Моё_приложение_unsafe-"))) return 1;
    QFile report(folder.filePath(files.first()));
    if (!report.open(QIODevice::ReadOnly)) return 1;
    const QByteArray text = report.readAll();
    if (!text.contains("previous context") || !text.contains("failure")) return 1;
    if (report.permissions() & (QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther | QFile::WriteOther)) return 1;
    const QString manualLog = QString::fromUtf8("First line\nЖурнал приложения\n");
    if (!ErrorLog::saveText(manualLog, &savedPath, &error) || !error.isEmpty() ||
        QFileInfo(savedPath).dir() != folder ||
        !QFileInfo(savedPath).fileName().startsWith(QString::fromUtf8("Моё_приложение_unsafe-"))) return 1;
    QFile manual(savedPath);
    if (!manual.open(QIODevice::ReadOnly) || manual.readAll() != manualLog.toUtf8() ||
        (manual.permissions() & (QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther | QFile::WriteOther))) return 1;

    if (!ErrorLog::startCoreChannel(&error)) return 1;
    ErrorLog::sendCoreLine(MEGLER_LOG_INFO, "core context");
    ErrorLog::sendCoreLine(MEGLER_LOG_ERROR, "core failure");
    QCoreApplication::processEvents();
    files = folder.entryList(QStringList() << "*.txt", QDir::Files);
    if (files.size() != 3) return 1;
    bool foundCore = false;
    for (const QString &name : files) {
        QFile f(folder.filePath(name));
        if (!f.open(QIODevice::ReadOnly)) return 1;
        const QByteArray data = f.readAll();
        if (data.contains("core failure") && data.contains("core context")) foundCore = true;
    }
    if (!foundCore) return 1;
    DiagnosticLog::installChildQtMessageHandler();
    qWarning("qt failure");
    if (folder.entryList(QStringList() << "*.txt", QDir::Files).size() != 4) return 1;
    std::fprintf(stderr, "Error logging OK\n");
    return 0;
}
