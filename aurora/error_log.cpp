#include "error_log.h"
#include "memory_log_bus.h"

#include <QDateTime>
#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QMutex>
#include <QMutexLocker>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTimer>
#include <QUuid>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <thread>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../core/include/megler.h"

namespace {
QMutex mutex;
QString directory;
QString recent;
QString memoryHistory;
QString memoryDetailed;
std::atomic<bool> memoryDetailedEnabled(false);
bool initialized = false;
struct RemoteHistory {
    QString safe;
    QString detailed;
};
QMap<QString, RemoteHistory> remoteHistory;
quint64 rejectedRemoteEvents = 0;
const int MaxRemoteClients = 32;
const int MaxRemoteSafe = 128 * 1024;
const int MaxRemoteDetailed = 512 * 1024;
const int MaxContext = 131072;
const int MaxJournalBytes = 4 * 1024 * 1024;
const int MaxJournalEntry = 65536;
const int MaxDetailedBytes = 16 * 1024 * 1024;
const char JournalTruncated[] = "[Earlier shared history was truncated.]\n";
const char DetailedTruncated[] = "[Earlier detailed output was truncated.]\n";
const int MaxSnapshotBytes = 96 * 1024 * 1024;
const quint32 SnapshotMagic = 0x41434c4c;
const quint32 SnapshotVersion = 1;
QString storagePath;
QString storageError;
quint64 storageRevision = 0;
quint64 storedRevision = 0;
std::atomic<bool> storageBusy(false);
struct StorageWorker {
    std::thread thread;
    ~StorageWorker() { if (thread.joinable()) thread.join(); }
} storageWorker;
StorageWorker standaloneFileWorker;
std::atomic<bool> standaloneFileBusy(false);
QString standaloneFilePath;
QString standaloneFileError;
struct Snapshot {
    QString history;
    QString detailed;
    bool detailedEnabled;
    QMap<QString, RemoteHistory> remote;
    quint64 rejected;
    quint64 revision;
};
int coreSender = -1;
int coreReceiver = -1;
std::atomic<quint64> coreDropped(0);
bool exitReportConnected = false;
thread_local bool recording = false;
struct RecordingGuard {
    RecordingGuard() { recording = true; }
    ~RecordingGuard() { recording = false; }
};

QString reportHeader()
{
    const QString product = QSysInfo::productType().toLower();
    const QString system = product.contains(QLatin1String("aurora")) ? QStringLiteral("Aurora")
        : product.contains(QLatin1String("sailfish")) ? QStringLiteral("Sailfish")
        : QSysInfo::kernelType() == QLatin1String("linux") ? QStringLiteral("Linux")
        : QStringLiteral("other");
    const QString version = QSysInfo::productVersion();
    const QRegularExpression versionPattern(QStringLiteral("\\A[0-9]+(?:\\.[0-9]+){0,4}\\z"));
    const QString architecture = QSysInfo::currentCpuArchitecture();
    const QStringList architectures = {QStringLiteral("arm64"), QStringLiteral("aarch64"),
                                       QStringLiteral("arm"), QStringLiteral("x86_64"),
                                       QStringLiteral("i386"), QStringLiteral("i686")};
    return QStringLiteral("Report format: 2\nBuild: %1 %2\n"
                          "System: %3\nArchitecture: %4\nQt: %5\nTime: %6 UTC\n\n")
        .arg(QString::fromLatin1(__DATE__)).arg(QString::fromLatin1(__TIME__))
        .arg(versionPattern.match(version).hasMatch() ? system + QLatin1Char(' ') + version : system)
        .arg(architectures.contains(architecture) ? architecture : QStringLiteral("other"))
        .arg(QString::fromLatin1(qVersion())).arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
}

QString safeEvent(const QString &source, const QString &message, bool error)
{
    static const QRegularExpression permitted(QStringLiteral(
        "^(?:APK path validation: rejected reason=(?:malformed-url|missing-scheme|content-uri|web-url|"
        "unsupported-scheme|file-url-has-host|file-url-has-query-or-fragment|not-local-file-url|nul-character|"
        "not-absolute|network-path|unclean-path|root-directory|unsupported-extension)"
        "|APK request: source=(?:runtime-open-uri|activation|direct|legacy-open-file) count=[0-9]{1,6}"
        "|APK request: source=runtime-open-uri rejected=missing-uri"
        "|APK request: rejected=argument-count-or-length"
        "|APK import: source=(?:external|manager)"
        "|APK import: format=(?:single|bundle)"
        "|APK library import: stage=(?:open|copy|inspect|install)"
        "|APK library import: result=(?:success|failed)"
        "|Standalone RPM: requested"
        "|Standalone RPM: preflight failed"
        "|Standalone RPM: builder finished exit=-?[0-9]{1,10} normal=[01] timed-out=[01]"
        "|Experimental GPU Canvas active; Android 2D draws into a shared GLES texture"
        "|Experimental GPU Canvas unavailable; using the CPU Canvas fallback"
        "|Experimental GPU Canvas presentation failed; using the CPU frame fallback"
        "|GPU Canvas (?:readback|upload) failed \\(OpenGL error 0x[0-9a-fA-F]{1,8}\\)"
        "|GPU Canvas performance: interval_ms=[0-9]{1,20} frames=[0-9]{1,20} failed=[0-9]{1,20} samples=[0-9]{1,20}"
        " producer_us=[0-9]{1,20} producer_max_us=[0-9]{1,20} qt_wait_us=[0-9]{1,20}"
        " qt_wait_max_us=[0-9]{1,20} readbacks=[0-9]{1,20} readback_samples=[0-9]{1,20}"
        " readback_us=[0-9]{1,20} readback_max_us=[0-9]{1,20}"
        "(?: readback_flush=[0-9]{1,20} readback_filter=[0-9]{1,20} readback_self=[0-9]{1,20}"
        " readback_gl_us=[0-9]{1,20} readback_convert_us=[0-9]{1,20}"
        "(?: readback_present=[0-9]{1,20}"
        "(?: pixel_submit_samples=[0-9]{1,20} pixel_submit_us=[0-9]{1,20} pixel_submit_max_us=[0-9]{1,20}"
        "(?: filter_fallback_mode=[0-9]{1,20} filter_fallback_blend=[0-9]{1,20}"
        " filter_fallback_self=[0-9]{1,20} filter_fallback_gles=[0-9]{1,20}"
        " filter_fallback_invalid=[0-9]{1,20} filter_draw_errors=[0-9]{1,20})?"
        "(?: readback_cache_flush=[0-9]{1,20} readback_cache_filter=[0-9]{1,20}"
        " readback_cache_self=[0-9]{1,20} readback_cache_present=[0-9]{1,20})?)?)?)?"
        "|GPU Canvas fallback \\(details omitted\\)"
        "|Preview process: pid=[0-9]{1,20} (?:started|result=(?:completed|nonzero|crashed|stopped))"
        "|Preview process: result=failed-to-start"
        "|Android log channel: [0-9]{1,20} events dropped"
        "|Log transport: [0-9]{1,20} early events unavailable while detailed state was unknown"
        "|Remote log collector unavailable"
        "|Application exited normally\\.|Runtime application initialized\\."
        "|Android runtime error \\(details omitted\\)|Standalone RPM error \\(details omitted\\)"
        "|Qt warning or error \\(details omitted\\)|Error \\(details omitted\\)"
        "|Android runtime error: (?:NullPointerException|ClassNotFoundException|NoClassDefFoundError|"
        "NoSuchMethodError|UnsatisfiedLinkError|SecurityException|IllegalArgumentException|"
        "IllegalStateException|OutOfMemoryError))\\z"));
    if (permitted.match(message).hasMatch()) return message;
    if (source == QLatin1String("Qt") &&
        message.startsWith(QLatin1String("Experimental GPU Canvas falling back to CPU:")))
        return QStringLiteral("GPU Canvas fallback (details omitted)");
    if (!error) return QString();
    if (source == QLatin1String("Android")) {
        static const char *exceptions[] = {
            "NullPointerException", "ClassNotFoundException", "NoClassDefFoundError",
            "NoSuchMethodError", "UnsatisfiedLinkError", "SecurityException",
            "IllegalArgumentException", "IllegalStateException", "OutOfMemoryError"
        };
        for (const char *name : exceptions)
            if (message.contains(QLatin1String(name)))
                return QStringLiteral("Android runtime error: %1").arg(QLatin1String(name));
        return QStringLiteral("Android runtime error (details omitted)");
    }
    if (source == QLatin1String("RPM")) return QStringLiteral("Standalone RPM error (details omitted)");
    if (source == QLatin1String("Qt")) return QStringLiteral("Qt warning or error (details omitted)");
    return QStringLiteral("Error (details omitted)");
}

void appendBounded(QString *buffer, const QString &entry, int maxChars, const char *marker)
{
    *buffer += entry;
    if (buffer->size() <= maxChars) return;
    const int cut = buffer->indexOf(QLatin1Char('\n'), buffer->size() - maxChars / 2);
    *buffer = QString::fromLatin1(marker) + buffer->mid(cut < 0 ? buffer->size() : cut + 1);
}

void changed()
{
    ++storageRevision;
}

bool writeString(QDataStream &stream, const QString &value)
{
    const QByteArray bytes = value.toUtf8();
    stream << quint32(bytes.size());
    return stream.writeRawData(bytes.constData(), bytes.size()) == bytes.size();
}

bool readString(QDataStream &stream, QString *value, int maxChars)
{
    quint32 length = 0;
    stream >> length;
    if (stream.status() != QDataStream::Ok || length > quint32(maxChars * 3) ||
        length > quint32(stream.device()->bytesAvailable())) return false;
    QByteArray bytes(int(length), '\0');
    if (stream.readRawData(bytes.data(), bytes.size()) != bytes.size()) return false;
    *value = QString::fromUtf8(bytes);
    return value->size() <= maxChars && value->toUtf8() == bytes;
}

Snapshot snapshot()
{
    QMutexLocker lock(&mutex);
    return Snapshot{memoryHistory, memoryDetailed, memoryDetailedEnabled, remoteHistory,
                    rejectedRemoteEvents, storageRevision};
}

bool saveSnapshot(const QString &path, const Snapshot &state, QString *error)
{
    const QByteArray temporary = QFile::encodeName(path + QStringLiteral(".pending"));
    const int fd = ::open(temporary.constData(), O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        *error = QString::fromLocal8Bit(std::strerror(errno));
        return false;
    }
    struct stat status;
    if (::fstat(fd, &status) < 0 || !S_ISREG(status.st_mode) || status.st_uid != ::geteuid() ||
        status.st_nlink != 1 || (status.st_mode & (S_IRWXG | S_IRWXO))) {
        ::close(fd);
        *error = QStringLiteral("Temporary log snapshot has unsafe ownership or permissions.");
        return false;
    }
    if (::ftruncate(fd, 0) < 0) {
        *error = QString::fromLocal8Bit(std::strerror(errno));
        ::close(fd);
        return false;
    }
    QFile file;
    if (!file.open(fd, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle)) {
        *error = file.errorString();
        ::close(fd);
        return false;
    }
    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_5_6);
    stream << SnapshotMagic << SnapshotVersion << quint8(state.detailedEnabled ? 1 : 0)
           << state.rejected << quint32(state.remote.size());
    bool ok = writeString(stream, state.history) && writeString(stream, state.detailed);
    for (auto it = state.remote.constBegin(); ok && it != state.remote.constEnd(); ++it)
        ok = writeString(stream, it.key()) && writeString(stream, it.value().safe) &&
             writeString(stream, it.value().detailed);
    if (!ok || stream.status() != QDataStream::Ok || file.pos() > MaxSnapshotBytes || !file.flush()) {
        *error = file.errorString().isEmpty() ? QStringLiteral("Cannot write the log snapshot.") : file.errorString();
        return false;
    }
    if (::fsync(fd) < 0) {
        *error = QString::fromLocal8Bit(std::strerror(errno));
        return false;
    }
    file.close();
    const QByteArray destination = QFile::encodeName(path);
    if (::rename(temporary.constData(), destination.constData()) < 0) {
        *error = QString::fromLocal8Bit(std::strerror(errno));
        return false;
    }
    const QByteArray folder = QFile::encodeName(QFileInfo(path).absolutePath());
    const int dirFd = ::open(folder.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dirFd < 0) {
        *error = QString::fromLocal8Bit(std::strerror(errno));
        return false;
    }
    const int synced = ::fsync(dirFd);
    const int syncError = errno;
    ::close(dirFd);
    if (synced < 0) {
        *error = QString::fromLocal8Bit(std::strerror(syncError));
        return false;
    }
    return true;
}

bool loadSnapshot(const QString &path, Snapshot *state, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = file.errorString();
        return false;
    }
    if (file.size() < 0 || file.size() > MaxSnapshotBytes) {
        *error = QStringLiteral("Invalid log snapshot size.");
        return false;
    }
    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_5_6);
    quint32 magic = 0, version = 0, count = 0;
    quint8 enabled = 0;
    stream >> magic >> version >> enabled >> state->rejected >> count;
    if (stream.status() != QDataStream::Ok || magic != SnapshotMagic || version != SnapshotVersion ||
        enabled > 1 || count > MaxRemoteClients ||
        !readString(stream, &state->history, MaxJournalBytes / int(sizeof(QChar))) ||
        !readString(stream, &state->detailed, MaxDetailedBytes / int(sizeof(QChar)))) {
        *error = QStringLiteral("Invalid log snapshot; the original was preserved.");
        return false;
    }
    state->detailedEnabled = enabled != 0;
    for (quint32 i = 0; i < count; ++i) {
        QString key;
        RemoteHistory history;
        if (!readString(stream, &key, 512) || key.isEmpty() || state->remote.contains(key) ||
            !readString(stream, &history.safe, MaxRemoteSafe) ||
            !readString(stream, &history.detailed, MaxRemoteDetailed)) {
            *error = QStringLiteral("Invalid log snapshot; the original was preserved.");
            return false;
        }
        state->remote.insert(key, history);
    }
    if (!file.atEnd()) {
        *error = QStringLiteral("Invalid log snapshot; the original was preserved.");
        return false;
    }
    return true;
}

void flushSnapshot()
{
    if (storagePath.isEmpty() || storageBusy.load(std::memory_order_acquire)) return;
    if (storageWorker.thread.joinable()) storageWorker.thread.join();
    const Snapshot state = snapshot();
    {
        QMutexLocker lock(&mutex);
        if (state.revision == storedRevision) return;
    }
    const QString path = storagePath;
    storageBusy.store(true, std::memory_order_release);
    storageWorker.thread = std::thread([path, state]() {
        QString error;
        const bool ok = saveSnapshot(path, state, &error);
        {
            QMutexLocker lock(&mutex);
            if (ok) {
                storedRevision = state.revision;
                storageError.clear();
            } else {
                storageError = error;
            }
        }
        storageBusy.store(false, std::memory_order_release);
    });
}

bool appendDetailed(const QString &source, const QString &message, QString *error)
{
    error->clear();
    if (memoryDetailedEnabled) {
        QString text = message;
        if (text.size() > MaxJournalEntry)
            text = QStringLiteral("[Earlier part of entry truncated.] ") + text.right(MaxJournalEntry);
        const QString entry = QStringLiteral("[%1 UTC] [pid=%2] [%3] %4\n")
            .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate))
            .arg(QCoreApplication::applicationPid()).arg(source, text);
        appendBounded(&memoryDetailed, entry, MaxDetailedBytes / int(sizeof(QChar)), DetailedTruncated);
        changed();
    }
    return true;
}

bool appendJournal(const QString &entry, QString *error)
{
    error->clear();
    QString bounded = entry;
    if (bounded.size() > MaxJournalEntry)
        bounded = QStringLiteral("[Earlier part of entry truncated.] ") + bounded.right(MaxJournalEntry);
    appendBounded(&memoryHistory, bounded, MaxJournalBytes / int(sizeof(QChar)), JournalTruncated);
    changed();
    return true;
}

QString sharedHistory()
{
    QString result = MemoryLogBus::statistics() + QLatin1Char('\n') +
                     QStringLiteral("Activity history:\n") + memoryHistory;
    if (!storageError.isEmpty())
        result.prepend(QStringLiteral("[Automatic log storage failed; recent events may be lost.]\n"));
    int number = 0;
    for (auto it = remoteHistory.constBegin(); it != remoteHistory.constEnd(); ++it) {
        result += QStringLiteral("\n[Remote source %1; events are untrusted]\n").arg(++number);
        result += it.value().safe;
    }
    if (rejectedRemoteEvents)
        result += QStringLiteral("[Remote events dropped because the source limit was reached: %1]\n").arg(rejectedRemoteEvents);
    return result + QLatin1Char('\n');
}

bool writeText(const QString &text, QString *path, QString *error, bool standaloneFile = false)
{
    if (path) path->clear();
    if (error) error->clear();
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QFileInfo documentsInfo(documents);
    if (documents.isEmpty() || !documentsInfo.isDir() || documentsInfo.canonicalFilePath().isEmpty()) {
        if (error) *error = QStringLiteral("Documents directory is unavailable.");
        return false;
    }
    const QString target = QDir(documentsInfo.canonicalFilePath()).filePath(QStringLiteral("AndroidCompatibilityLayer-Error-Logs"));
    if (QFileInfo(target).isSymLink() || !QDir().mkpath(target) || !QFileInfo(target).isDir()) {
        if (error) *error = QStringLiteral("Cannot create the error log directory.");
        return false;
    }
    directory = target;
    if (directory.isEmpty() || QFileInfo(directory).isSymLink() || !QFileInfo(directory).isDir()) {
        if (error) *error = QCoreApplication::translate("ErrorLog", "Log directory is unavailable.");
        return false;
    }
    QLockFile allocationLock(QDir(directory).filePath(QStringLiteral(".standalone-diagnostic-allocation.lock")));
    if (standaloneFile) {
        if (!allocationLock.tryLock(0)) {
            if (error) *error = QStringLiteral("Cannot reserve a standalone diagnostic file.");
            return false;
        }
        const QStringList previous = QDir(directory).entryList(
            QStringList() << QStringLiteral("StandaloneDiagnostic-*.txt"), QDir::Files | QDir::Hidden);
        if (previous.size() >= 8) {
            if (error) *error = QStringLiteral("Standalone diagnostic file limit reached; move or remove old test files.");
            return false;
        }
    }
    QString unique = QUuid::createUuid().toString();
    unique.remove(QLatin1Char('{'));
    unique.remove(QLatin1Char('}'));
    const QString name = (standaloneFile ? QStringLiteral("StandaloneDiagnostic-")
                                         : QStringLiteral("AndroidCompatibilityLayer-"))
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

void flushStandaloneFile()
{
    if (standaloneFilePath.isEmpty() || standaloneFileBusy.load(std::memory_order_acquire)) return;
    if (standaloneFileWorker.thread.joinable()) standaloneFileWorker.thread.join();
    QString text, error;
    if (!ErrorLog::detailedReport(&text, &error)) {
        QMutexLocker lock(&mutex);
        standaloneFileError = error;
        return;
    }
    const QString path = standaloneFilePath;
    standaloneFileBusy.store(true, std::memory_order_release);
    standaloneFileWorker.thread = std::thread([path, text]() {
        QSaveFile file(path);
        const QByteArray contents = text.toUtf8();
        const bool ok = file.open(QIODevice::WriteOnly) &&
            file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) &&
            file.write(contents) == contents.size() && file.commit();
        const QString error = ok ? QString() : file.errorString();
        {
            QMutexLocker lock(&mutex);
            standaloneFileError = error;
        }
        standaloneFileBusy.store(false, std::memory_order_release);
    });
}
}

bool ErrorLog::initialize(const QString &application, QString *error, const QString &sharedRoot)
{
    QMutexLocker lock(&mutex);
    if (error) error->clear();
    Q_UNUSED(sharedRoot);
    initialized = true;
    if (!exitReportConnected && QCoreApplication::instance()) {
        QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, []() {
            ErrorLog::record(QStringLiteral("Lifecycle"), QStringLiteral("Application exited normally."), false);
        });
        exitReportConnected = true;
    }
    QString journalError;
    if (!appendJournal(QStringLiteral("[%1 UTC] [pid=%2] Process started: %3\n")
            .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate))
            .arg(QCoreApplication::applicationPid())
            .arg(application == QLatin1String("Android App Manager")
                     ? QStringLiteral("manager") : QStringLiteral("android-runtime")), &journalError))
        std::fprintf(stderr, "Could not write shared diagnostic history: %s\n", qPrintable(journalError));
    return true;
}

void ErrorLog::setApplicationName(const QString &application)
{
    Q_UNUSED(application);
    record(QStringLiteral("Lifecycle"), QStringLiteral("Runtime application initialized."), false);
}

void ErrorLog::record(const QString &source, const QString &message, bool error)
{
    if (recording) {
        std::fputs("Could not save recursive diagnostic message.\n", stderr);
        return;
    }
    RecordingGuard guard;
    QMutexLocker lock(&mutex);
    if (message.startsWith(QLatin1String("GPU Canvas performance: ")) && !detailedCaptureEnabled())
        return;
    QString detailedError;
    if (!appendDetailed(source, message, &detailedError)) {
        static bool reported = false;
        if (!reported) {
            std::fprintf(stderr, "Could not write detailed diagnostic log: %s\n", qPrintable(detailedError));
            reported = true;
        }
    }
    const QString safe = safeEvent(source, message, error);
    if (MemoryLogBus::clientActive())
        MemoryLogBus::record(source, message, safe, error);
    if (safe.isEmpty() || !initialized) return;
    const QString entry = QStringLiteral("[%1 UTC] [pid=%2] %3\n")
        .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate))
        .arg(QCoreApplication::applicationPid()).arg(safe);
    recent += entry;
    QString journalError;
    if (!appendJournal(entry, &journalError))
        std::fprintf(stderr, "Could not write shared diagnostic history: %s\n", qPrintable(journalError));
    if (recent.size() > MaxContext) {
        int cut = recent.size() - MaxContext;
        if (cut < recent.size() && recent.at(cut).isLowSurrogate()) ++cut;
        recent.remove(0, cut);
    }
}

QString ErrorLog::displayText()
{
    QMutexLocker lock(&mutex);
    if (!initialized) return QString();
    return reportHeader() + sharedHistory();
}

QString ErrorLog::report()
{
    QMutexLocker lock(&mutex);
    if (!initialized) return QString();
    return reportHeader() + sharedHistory() + QStringLiteral("Current process context:\n") + recent;
}

bool ErrorLog::saveReport(QString *path, QString *error)
{
    if (recording) {
        if (path) path->clear();
        if (error) *error = QCoreApplication::translate("ErrorLog", "Log saving is already in progress.");
        return false;
    }
    RecordingGuard guard;
    QMutexLocker lock(&mutex);
    if (!initialized) {
        if (path) path->clear();
        if (error) *error = QStringLiteral("Log capture is unavailable.");
        return false;
    }
    return writeText(reportHeader() + sharedHistory() + QStringLiteral("Current process context:\n") + recent,
                     path, error);
}

bool ErrorLog::detailedEnabled(bool *enabled, QString *error)
{
    if (error) error->clear();
    if (!enabled || !error) return false;
    *enabled = false;
    QMutexLocker lock(&mutex);
    *enabled = memoryDetailedEnabled;
    return true;
}

bool ErrorLog::detailedCaptureEnabled()
{
    return memoryDetailedEnabled.load(std::memory_order_relaxed) || MemoryLogBus::clientDetailedEnabled();
}

bool ErrorLog::setDetailedEnabled(bool enabled, QString *error)
{
    if (!error) return false;
    error->clear();
    {
        QMutexLocker lock(&mutex);
        if (memoryDetailedEnabled == enabled) return true;
        memoryDetailed.clear();
        for (auto it = remoteHistory.begin(); it != remoteHistory.end(); ++it) it.value().detailed.clear();
        memoryDetailedEnabled = enabled;
        changed();
    }
    if (!storagePath.isEmpty()) {
        if (storageWorker.thread.joinable()) storageWorker.thread.join();
        flushSnapshot();
        if (storageWorker.thread.joinable()) storageWorker.thread.join();
        QMutexLocker lock(&mutex);
        if (!storageError.isEmpty()) {
            *error = storageError;
            return false;
        }
    }
    return true;
}

bool ErrorLog::detailedReport(QString *text, QString *error)
{
    if (!text || !error) return false;
    text->clear();
    error->clear();
    QMutexLocker lock(&mutex);
    if (!memoryDetailedEnabled) {
        *error = QStringLiteral("Detailed logging is not enabled.");
        return false;
    }
    *text = reportHeader() + MemoryLogBus::statistics() +
            QStringLiteral("\nDetailed log: may contain private data.\n\n") + memoryDetailed;
    if (!storageError.isEmpty())
        text->prepend(QStringLiteral("[Automatic log storage failed; recent events may be lost.]\n"));
    int number = 0;
    for (auto it = remoteHistory.constBegin(); it != remoteHistory.constEnd(); ++it)
        *text += QStringLiteral("\n[Remote source %1; events are untrusted]\n").arg(++number) + it.value().detailed;
    if (rejectedRemoteEvents)
        *text += QStringLiteral("[Remote events dropped because the source limit was reached: %1]\n").arg(rejectedRemoteEvents);
    return true;
}

bool ErrorLog::saveDetailedReport(QString *path, QString *error)
{
    if (path) path->clear();
    QString text;
    if (!detailedReport(&text, error)) return false;
    QMutexLocker lock(&mutex);
    return writeText(text, path, error);
}

bool ErrorLog::startCollector(QString *error)
{
    const bool started = MemoryLogBus::startServer(
        [](const QString &identity, const QString &source, const QString &message, bool isError, quint64 dropped) {
            QMutexLocker lock(&mutex);
            if (!remoteHistory.contains(identity) && remoteHistory.size() >= MaxRemoteClients) {
                const quint64 available = std::numeric_limits<quint64>::max() - rejectedRemoteEvents;
                const quint64 count = message.isEmpty() ? dropped
                    : dropped == std::numeric_limits<quint64>::max() ? dropped : dropped + 1;
                rejectedRemoteEvents += qMin(available, count);
                changed();
                return;
            }
            RemoteHistory &history = remoteHistory[identity];
            changed();
            const QString time = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
            if (dropped) {
                const QString note = QStringLiteral("[%1 UTC] [Remote log events dropped: %2]\n").arg(time).arg(dropped);
                appendBounded(&history.safe, note, MaxRemoteSafe, JournalTruncated);
                if (memoryDetailedEnabled)
                    appendBounded(&history.detailed, note, MaxRemoteDetailed, DetailedTruncated);
            }
            if (message.isEmpty()) return;
            if (!memoryDetailedEnabled &&
                message.startsWith(QLatin1String("GPU Canvas performance: "))) return;
            const QString safe = safeEvent(source, message, isError);
            if (!safe.isEmpty())
                appendBounded(&history.safe, QStringLiteral("[%1 UTC] %2\n").arg(time, safe),
                              MaxRemoteSafe, JournalTruncated);
            if (memoryDetailedEnabled)
                appendBounded(&history.detailed, QStringLiteral("[%1 UTC] [%2] %3\n").arg(time, source, message),
                              MaxRemoteDetailed, DetailedTruncated);
        },
        []() {
            QMutexLocker lock(&mutex);
            return memoryDetailedEnabled.load(std::memory_order_relaxed);
        }, error);
    if (!started) {
        {
            QMutexLocker lock(&mutex);
            storageError = QStringLiteral("Log collector unavailable; automatic storage is disabled.");
        }
        record(QStringLiteral("IPC"), QStringLiteral("Remote log collector unavailable"), true);
    }
    return started;
}

bool ErrorLog::startForwarding(QString *error)
{
    if (!MemoryLogBus::startClient(error)) return false;
    MemoryLogBus::enqueue(QStringLiteral("Lifecycle"), QStringLiteral("Runtime application initialized."), false);
    return true;
}

bool ErrorLog::startPersistence(const QString &privateRoot, QString *error)
{
    if (!error) return false;
    error->clear();
    auto fail = [error](const QString &message) {
        *error = message;
        QMutexLocker lock(&mutex);
        storageError = message;
        return false;
    };
    if (!QCoreApplication::instance() || privateRoot.isEmpty() || !storagePath.isEmpty())
        return fail(QStringLiteral("Private log storage is unavailable."));
    if (!QDir().mkpath(privateRoot))
        return fail(QStringLiteral("Cannot create the private application directory."));
    const QFileInfo root(privateRoot);
    if (!root.isDir() || root.isSymLink() || root.canonicalFilePath().isEmpty())
        return fail(QStringLiteral("Invalid private application directory."));
    const QString folder = QDir(root.canonicalFilePath()).filePath(QStringLiteral("diagnostic-log"));
    const QByteArray nativeFolder = QFile::encodeName(folder);
    if (::mkdir(nativeFolder.constData(), 0700) < 0 && errno != EEXIST)
        return fail(QString::fromLocal8Bit(std::strerror(errno)));
    struct stat status;
    if (::lstat(nativeFolder.constData(), &status) < 0 || !S_ISDIR(status.st_mode) ||
        status.st_uid != ::geteuid() || (status.st_mode & (S_IRWXG | S_IRWXO)))
        return fail(QStringLiteral("Private log directory has unsafe ownership or permissions."));
    const QString path = QDir(folder).filePath(QStringLiteral("manager-state-v1.dat"));
    const QByteArray nativePath = QFile::encodeName(path);
    Snapshot old;
    if (::lstat(nativePath.constData(), &status) == 0) {
        if (!S_ISREG(status.st_mode) || status.st_uid != ::geteuid() || status.st_nlink != 1 ||
            (status.st_mode & (S_IRWXG | S_IRWXO)))
            return fail(QStringLiteral("Log snapshot has unsafe ownership or permissions."));
        if (!loadSnapshot(path, &old, error)) return fail(*error);
        QMutexLocker lock(&mutex);
        const QString current = memoryHistory;
        memoryHistory = old.history;
        appendBounded(&memoryHistory, current, MaxJournalBytes / int(sizeof(QChar)), JournalTruncated);
        memoryDetailed = old.detailed;
        memoryDetailedEnabled = old.detailedEnabled;
        remoteHistory = old.remote;
        rejectedRemoteEvents = old.rejected;
        changed();
    } else if (errno != ENOENT) {
        return fail(QString::fromLocal8Bit(std::strerror(errno)));
    }
    storagePath = path;
    QTimer *timer = new QTimer(QCoreApplication::instance());
    timer->setInterval(5000);
    QObject::connect(timer, &QTimer::timeout, timer, []() { flushSnapshot(); });
    QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, []() {
        if (storageWorker.thread.joinable()) storageWorker.thread.join();
        flushSnapshot();
        if (storageWorker.thread.joinable()) storageWorker.thread.join();
    });
    timer->start();
    return true;
}

QString ErrorLog::persistenceError()
{
    QMutexLocker lock(&mutex);
    return storageError;
}

bool ErrorLog::startStandaloneDiagnosticFile(QString *path, QString *error)
{
    if (path) path->clear();
    if (!path || !error) return false;
    error->clear();
    if (!QCoreApplication::instance() || !standaloneFilePath.isEmpty()) {
        *error = QStringLiteral("Standalone diagnostic file is unavailable.");
    } else if (setDetailedEnabled(true, error)) {
        QString text;
        if (!detailedReport(&text, error) || !writeText(text, path, error, true)) {
            QMutexLocker lock(&mutex);
            standaloneFileError = *error;
            return false;
        }
        standaloneFilePath = *path;
        QTimer *timer = new QTimer(QCoreApplication::instance());
        timer->setInterval(5000);
        QObject::connect(timer, &QTimer::timeout, timer, []() { flushStandaloneFile(); });
        QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, []() {
            if (standaloneFileWorker.thread.joinable()) standaloneFileWorker.thread.join();
            flushStandaloneFile();
            if (standaloneFileWorker.thread.joinable()) standaloneFileWorker.thread.join();
        });
        timer->start();
        return true;
    }
    QMutexLocker lock(&mutex);
    standaloneFileError = *error;
    return false;
}

QString ErrorLog::standaloneDiagnosticFileError()
{
    QMutexLocker lock(&mutex);
    return standaloneFileError;
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
            const ssize_t count = ::recv(coreReceiver, bytes, sizeof(bytes), MSG_DONTWAIT | MSG_TRUNC);
            if (count < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK)
                    std::fprintf(stderr, "Error log channel read failed: %s\n", std::strerror(errno));
                break;
            }
            if (count < ssize_t(sizeof(int)) || count > ssize_t(sizeof(bytes))) {
                coreDropped.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            int level;
            std::memcpy(&level, bytes, sizeof(level));
            ErrorLog::record(QStringLiteral("Android"), QString::fromUtf8(bytes + sizeof(level),
                             int(count - sizeof(level))), level >= MEGLER_LOG_ERROR);
        }
    });
    QTimer *lossTimer = new QTimer(notifier);
    lossTimer->setInterval(250);
    QObject::connect(lossTimer, &QTimer::timeout, notifier, []() {
        const quint64 dropped = coreDropped.exchange(0, std::memory_order_relaxed);
        if (dropped)
            ErrorLog::record(QStringLiteral("Android"),
                             QStringLiteral("Android log channel: %1 events dropped").arg(dropped), false);
    });
    lossTimer->start();
    return true;
}

void ErrorLog::sendCoreLine(int level, const char *line)
{
    if (coreSender < 0 || !line) return;
    const size_t length = std::strlen(line);
    if (length > 4096 - sizeof(level)) {
        coreDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    iovec parts[2] = {{&level, sizeof(level)}, {const_cast<char *>(line), length}};
    msghdr message = {};
    message.msg_iov = parts;
    message.msg_iovlen = 2;
    if (::sendmsg(coreSender, &message, MSG_DONTWAIT | MSG_NOSIGNAL) < 0)
        coreDropped.fetch_add(1, std::memory_order_relaxed);
}
