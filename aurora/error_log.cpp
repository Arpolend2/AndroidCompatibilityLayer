#include "error_log.h"

#include <QDateTime>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QSaveFile>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QUuid>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

#include "../core/include/megler.h"

namespace {
QMutex mutex;
QString directory;
QString applicationName;
QString recent;
const int MaxContext = 131072;
int coreSender = -1;
int coreReceiver = -1;
thread_local bool recording = false;
struct RecordingGuard {
    RecordingGuard() { recording = true; }
    ~RecordingGuard() { recording = false; }
};

QString safeName(const QString &name)
{
    QString result;
    for (QChar ch : name) {
        if (result.size() >= 80) break;
        result += ch.isLetterOrNumber() || ch == QLatin1Char('-') || ch == QLatin1Char('_')
            ? ch : QLatin1Char('_');
    }
    return result.isEmpty() ? QStringLiteral("AndroidCompatibilityLayer") : result;
}

bool writeText(const QString &text, QString *path, QString *error)
{
    if (path) path->clear();
    if (error) error->clear();
    if (directory.isEmpty() || QFileInfo(directory).isSymLink() || !QFileInfo(directory).isDir()) {
        if (error) *error = QCoreApplication::translate("ErrorLog", "Log directory is unavailable.");
        return false;
    }
    QString unique = QUuid::createUuid().toString();
    unique.remove(QLatin1Char('{'));
    unique.remove(QLatin1Char('}'));
    const QString name = safeName(applicationName) + QLatin1Char('-')
        + QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"))
        + QLatin1Char('-') + unique + QStringLiteral(".txt");
    const QString output = QDir(directory).filePath(name);
    QSaveFile file(output);
    const QByteArray contents = text.toUtf8();
    if (!file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
        file.write(contents) != contents.size() || !file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    if (path) *path = output;
    return true;
}
}

bool ErrorLog::initialize(const QString &application, QString *error)
{
    QMutexLocker lock(&mutex);
    if (error) error->clear();
    directory.clear();
    recent.clear();
    applicationName = application;
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QFileInfo documentsInfo(documents);
    if (documents.isEmpty() || !documentsInfo.isDir() ||
        documentsInfo.canonicalFilePath().isEmpty()) {
        if (error) *error = QStringLiteral("Documents directory is unavailable.");
        return false;
    }
    const QString target = QDir(documentsInfo.canonicalFilePath()).filePath(QStringLiteral("AndroidCompatibilityLayer-Error-Logs"));
    if (QFileInfo(target).isSymLink() || !QDir().mkpath(target) || !QFileInfo(target).isDir()) {
        if (error) *error = QStringLiteral("Cannot create the error log directory.");
        return false;
    }
    directory = target;
    return true;
}

void ErrorLog::setApplicationName(const QString &application)
{
    QMutexLocker lock(&mutex);
    applicationName = application;
}

void ErrorLog::record(const QString &source, const QString &message, bool error)
{
    if (recording) {
        std::fputs("Could not save recursive diagnostic message.\n", stderr);
        return;
    }
    RecordingGuard guard;
    QMutexLocker lock(&mutex);
    if (directory.isEmpty()) return;
    recent += QStringLiteral("[%1] %2\n").arg(source, message);
    if (recent.size() > MaxContext) {
        int cut = recent.size() - MaxContext;
        if (cut < recent.size() && recent.at(cut).isLowSurrogate()) ++cut;
        recent.remove(0, cut);
    }
    if (!error) return;
    const QString contents = QStringLiteral("Application: %1\nTime: %2 UTC\n\n")
        .arg(applicationName, QDateTime::currentDateTimeUtc().toString(Qt::ISODate)) + recent;
    QString writeError;
    if (!writeText(contents, nullptr, &writeError))
        std::fprintf(stderr, "Could not save error log: %s\n", qPrintable(writeError));
}

bool ErrorLog::saveText(const QString &text, QString *path, QString *error)
{
    if (recording) {
        if (path) path->clear();
        if (error) *error = QCoreApplication::translate("ErrorLog", "Log saving is already in progress.");
        return false;
    }
    RecordingGuard guard;
    QMutexLocker lock(&mutex);
    return writeText(text, path, error);
}

bool ErrorLog::startCoreChannel(QString *error)
{
    if (error) error->clear();
    if (coreSender >= 0) return true;
    int sockets[2];
    if (::socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets) < 0) {
        if (error) *error = QString::fromLocal8Bit(std::strerror(errno));
        return false;
    }
    coreSender = sockets[0];
    coreReceiver = sockets[1];
    QSocketNotifier *notifier = new QSocketNotifier(coreReceiver, QSocketNotifier::Read);
    QObject::connect(notifier, &QSocketNotifier::activated, notifier, [](int) {
        char bytes[4096];
        for (int i = 0; i < 256; ++i) {
            const ssize_t count = ::recv(coreReceiver, bytes, sizeof(bytes), MSG_DONTWAIT);
            if (count < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK)
                    std::fprintf(stderr, "Error log channel read failed: %s\n", std::strerror(errno));
                break;
            }
            if (count < ssize_t(sizeof(int))) continue;
            int level;
            std::memcpy(&level, bytes, sizeof(level));
            ErrorLog::record(QStringLiteral("Android"), QString::fromUtf8(bytes + sizeof(level),
                             int(count - sizeof(level))), level >= MEGLER_LOG_ERROR);
        }
    });
    return true;
}

void ErrorLog::sendCoreLine(int level, const char *line)
{
    if (coreSender < 0 || !line) return;
    iovec parts[2] = {{&level, sizeof(level)}, {const_cast<char *>(line), std::strlen(line)}};
    msghdr message = {};
    message.msg_iov = parts;
    message.msg_iovlen = 2;
    if (::sendmsg(coreSender, &message, MSG_DONTWAIT | MSG_NOSIGNAL) < 0) {
        const char warning[] = "Could not send Android output to the error log.\n";
        ::write(STDERR_FILENO, warning, sizeof(warning) - 1);
    }
}
