#include "error_log.h"
#include "memory_log_bus.h"

#include <QDateTime>
#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLockFile>
#include <QMutex>
#include <QMutexLocker>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTimer>
#include <QUuid>
#include <QVector>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <thread>
#include <dirent.h>
#include <sys/file.h>
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
const char ExportDirectoryName[] = "AndroidCompatibilityLayer-Error-Logs";
const char SharedStandaloneDirectoryName[] = "AndroidCompatibilityLayer-Standalone-Logs";
const char JournalTruncated[] = "[Earlier shared history was truncated.]\n";
const char DetailedTruncated[] = "[Earlier detailed output was truncated.]\n";
const int MaxStandaloneSnapshots = 32;
const int MaxStandaloneSnapshotBytes = 256 * 1024;
const int MaxStandaloneSnapshotEvents = 512;
const int StandaloneStartupEvents = 32;
const int MaxStandalonePendingBytes = 128 * 1024;
const int MaxStandaloneMessageChars = 4096;
const int MaxStandaloneExportCursors = 512;
const int MaxSnapshotBytes = 96 * 1024 * 1024;
const quint32 SnapshotMagic = 0x41434c4c;
const quint32 SnapshotVersion = 2;
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
    QMap<QString, quint64> standaloneExported;
    quint64 rejected;
    quint64 revision;
};
struct StandaloneEvent {
    quint64 sequence;
    QString time;
    QString source;
    QString message;
    bool error;
};
struct StandaloneSnapshot {
    QString fileName;
    QString instance;
    QString applicationId;
    QVector<StandaloneEvent> events;
    quint64 dropped = 0;
    int lockFd = -1;
    dev_t device = 0;
    ino_t inode = 0;
};
struct StandaloneWriter {
    bool active = false;
    QString directory;
    QString instance;
    QString applicationId;
    QString error;
    QVector<StandaloneEvent> pending;
    quint64 nextSequence = 0;
    quint64 dropped = 0;
};
StandaloneWriter standaloneWriter;
QString sharedStandaloneDirectory;
QString sharedStandaloneError;
bool standaloneSnapshotMode = false;
QMap<QString, quint64> exportedStandaloneSequences;
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
        " readback_cache_self=[0-9]{1,20} readback_cache_present=[0-9]{1,20}"
        "(?: canvas_copies_gpu=[0-9]{1,20})?)?)?)?)?"
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

bool validStandaloneApplicationId(const QString &applicationId)
{
    static const QRegularExpression pattern(
        QStringLiteral("\\Aru\\.androidcompatibilitylayer\\.Android[0-9a-f]{24}\\z"));
    return pattern.match(applicationId).hasMatch();
}

QString newStandaloneInstanceId()
{
    QString instance = QUuid::createUuid().toString();
    instance.remove(QLatin1Char('{'));
    instance.remove(QLatin1Char('}'));
    instance.remove(QLatin1Char('-'));
    return instance;
}

bool validStandaloneFileName(const QString &fileName, QString *instance)
{
    static const QRegularExpression pattern(QStringLiteral("\\Astandalone-([0-9a-f]{32})\\.json\\z"));
    const QRegularExpressionMatch match = pattern.match(fileName);
    if (!match.hasMatch()) return false;
    if (instance) *instance = match.captured(1);
    return true;
}

bool openStandaloneDirectory(const QString &sharedRoot, bool create, QString *directoryPath,
                            int *directoryFd, QString *error)
{
    if (directoryFd) *directoryFd = -1;
    if (error) error->clear();
    const QFileInfo rootInfo(sharedRoot);
    const QString root = rootInfo.canonicalFilePath();
    if (sharedRoot.isEmpty() || !rootInfo.isDir() || root.isEmpty()) {
        if (error) *error = QStringLiteral("Organization files directory is unavailable.");
        return false;
    }
    const QString target = QDir(root).filePath(QLatin1String(SharedStandaloneDirectoryName));
    const QByteArray nativeTarget = QFile::encodeName(target);
    if (create && ::mkdir(nativeTarget.constData(), 0775) < 0 && errno != EEXIST) {
        if (error) *error = QString::fromLocal8Bit(std::strerror(errno));
        return false;
    }
    struct stat status;
    if (::lstat(nativeTarget.constData(), &status) < 0 || !S_ISDIR(status.st_mode)) {
        if (error) *error = QStringLiteral("Standalone snapshot directory is not a regular directory.");
        return false;
    }
    const QFileInfo targetInfo(target);
    if (targetInfo.canonicalFilePath() != QDir::cleanPath(target)) {
        if (error) *error = QStringLiteral("Standalone snapshot directory is not stable.");
        return false;
    }
    const int fd = ::open(nativeTarget.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        if (error) *error = QString::fromLocal8Bit(std::strerror(errno));
        return false;
    }
    if (directoryPath) *directoryPath = target;
    if (directoryFd) *directoryFd = fd;
    else ::close(fd);
    return true;
}

bool standaloneSnapshotNames(int directoryFd, QVector<QString> *names, QString *error)
{
    names->clear();
    if (error) error->clear();
    const int duplicate = ::dup(directoryFd);
    if (duplicate < 0) {
        if (error) *error = QString::fromLocal8Bit(std::strerror(errno));
        return false;
    }
    DIR *directory = ::fdopendir(duplicate);
    if (!directory) {
        if (error) *error = QString::fromLocal8Bit(std::strerror(errno));
        ::close(duplicate);
        return false;
    }
    errno = 0;
    while (dirent *entry = ::readdir(directory)) {
        const QString name = QFile::decodeName(entry->d_name);
        if (validStandaloneFileName(name, nullptr)) names->append(name);
        errno = 0;
    }
    const int readError = errno;
    ::closedir(directory);
    if (readError) {
        names->clear();
        if (error) *error = QString::fromLocal8Bit(std::strerror(readError));
        return false;
    }
    std::sort(names->begin(), names->end());
    return true;
}

StandaloneEvent dropStandaloneEvent(QVector<StandaloneEvent> *events)
{
    // Keep an initial startup window and prefer discarding non-errors thereafter.
    const int firstCandidate = events->size() > StandaloneStartupEvents ? StandaloneStartupEvents : 0;
    int index = firstCandidate;
    while (index < events->size() && events->at(index).error) ++index;
    return events->takeAt(index < events->size() ? index : firstCandidate);
}

int lockStandaloneSnapshot(int directoryFd, const QString &fileName, QString *error)
{
    if (error) error->clear();
    QString instance;
    if (!validStandaloneFileName(fileName, &instance)) {
        if (error) *error = QStringLiteral("Invalid standalone snapshot name.");
        return -1;
    }
    const QByteArray lockName = QFile::encodeName(QStringLiteral("standalone-%1.lock").arg(instance));
    const int fd = ::openat(directoryFd, lockName.constData(),
                            O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) {
        if (error) *error = QString::fromLocal8Bit(std::strerror(errno));
        return -1;
    }
    struct stat status;
    if (::fstat(fd, &status) < 0 || !S_ISREG(status.st_mode) || status.st_nlink != 1) {
        if (error) *error = QStringLiteral("Standalone snapshot lock is not a regular file.");
        ::close(fd);
        return -1;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) < 0) {
        const int lockError = errno;
        ::close(fd);
        if (lockError == EWOULDBLOCK || lockError == EAGAIN) return -2;
        if (error) *error = QString::fromLocal8Bit(std::strerror(lockError));
        return -1;
    }
    return fd;
}

bool readStandaloneFile(int directoryFd, const QString &fileName, QByteArray *contents,
                        bool *exists, QString *error, struct stat *fileStatus = nullptr)
{
    if (contents) contents->clear();
    if (exists) *exists = false;
    if (error) error->clear();
    if (!validStandaloneFileName(fileName, nullptr)) {
        if (error) *error = QStringLiteral("Invalid standalone snapshot name.");
        return false;
    }
    const QByteArray nativeName = QFile::encodeName(fileName);
    const int fd = ::openat(directoryFd, nativeName.constData(), O_RDONLY | O_NOFOLLOW |
                            O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        if (errno == ENOENT) return true;
        if (error) *error = QString::fromLocal8Bit(std::strerror(errno));
        return false;
    }
    struct stat status;
    if (::fstat(fd, &status) < 0 || !S_ISREG(status.st_mode) || status.st_nlink != 1 ||
        status.st_size < 0 || status.st_size > MaxStandaloneSnapshotBytes) {
        ::close(fd);
        if (error) *error = QStringLiteral("Standalone snapshot is not a bounded ordinary file.");
        return false;
    }
    QByteArray bytes;
    bytes.resize(int(status.st_size));
    int offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::read(fd, bytes.data() + offset, size_t(bytes.size() - offset));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            const int readError = errno;
            ::close(fd);
            if (error) *error = count == 0 ? QStringLiteral("Standalone snapshot changed while reading.")
                                           : QString::fromLocal8Bit(std::strerror(readError));
            return false;
        }
        offset += int(count);
    }
    char extra;
    const ssize_t extraCount = ::read(fd, &extra, 1);
    const int extraError = errno;
    ::close(fd);
    if (extraCount != 0) {
        if (error) *error = extraCount > 0 ? QStringLiteral("Standalone snapshot exceeded its size limit.")
                                           : QString::fromLocal8Bit(std::strerror(extraError));
        return false;
    }
    if (contents) *contents = bytes;
    if (exists) *exists = true;
    if (fileStatus) *fileStatus = status;
    return true;
}

bool writeStandaloneFile(int directoryFd, const QString &fileName, const QByteArray &contents,
                         QString *error)
{
    if (error) error->clear();
    QString instance;
    if (!validStandaloneFileName(fileName, &instance) || contents.size() > MaxStandaloneSnapshotBytes) {
        if (error) *error = QStringLiteral("Standalone snapshot exceeds its safe limits.");
        return false;
    }
    const QString temporaryName = QStringLiteral(".standalone-%1-%2.pending")
        .arg(instance, newStandaloneInstanceId());
    const QByteArray nativeTemporary = QFile::encodeName(temporaryName);
    const int fd = ::openat(directoryFd, nativeTemporary.constData(),
                            O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) {
        if (error) *error = QString::fromLocal8Bit(std::strerror(errno));
        return false;
    }
    bool ok = true;
    int offset = 0;
    while (offset < contents.size()) {
        const ssize_t count = ::write(fd, contents.constData() + offset,
                                      size_t(contents.size() - offset));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            ok = false;
            if (error) *error = QString::fromLocal8Bit(std::strerror(errno));
            break;
        }
        offset += int(count);
    }
    if (ok && ::fsync(fd) < 0) {
        ok = false;
        if (error) *error = QString::fromLocal8Bit(std::strerror(errno));
    }
    const int closeResult = ::close(fd);
    if (ok && closeResult < 0) {
        ok = false;
        if (error) *error = QString::fromLocal8Bit(std::strerror(errno));
    }
    const QByteArray nativeName = QFile::encodeName(fileName);
    if (ok && ::renameat(directoryFd, nativeTemporary.constData(), directoryFd, nativeName.constData()) < 0) {
        ok = false;
        if (error) *error = QString::fromLocal8Bit(std::strerror(errno));
    }
    if (ok && ::fsync(directoryFd) < 0) {
        ok = false;
        if (error) *error = QString::fromLocal8Bit(std::strerror(errno));
    }
    if (!ok) ::unlinkat(directoryFd, nativeTemporary.constData(), 0);
    return ok;
}

bool removeStandaloneFile(int directoryFd, const QString &fileName, QString *error,
                          const StandaloneSnapshot *expected = nullptr)
{
    if (error) error->clear();
    if (!validStandaloneFileName(fileName, nullptr)) {
        if (error) *error = QStringLiteral("Invalid standalone snapshot name.");
        return false;
    }
    const QByteArray nativeName = QFile::encodeName(fileName);
    struct stat status;
    if (::fstatat(directoryFd, nativeName.constData(), &status, AT_SYMLINK_NOFOLLOW) < 0) {
        if (errno == ENOENT) return true;
        if (error) *error = QString::fromLocal8Bit(std::strerror(errno));
        return false;
    }
    if (!S_ISREG(status.st_mode) || status.st_nlink != 1) {
        if (error) *error = QStringLiteral("Standalone snapshot is not an ordinary file.");
        return false;
    }
    if (expected && (quint64(status.st_dev) != quint64(expected->device) ||
                     quint64(status.st_ino) != quint64(expected->inode))) {
        if (error) *error = QStringLiteral("Standalone snapshot was replaced during export.");
        return false;
    }
    if (::unlinkat(directoryFd, nativeName.constData(), 0) < 0 && errno != ENOENT) {
        if (error) *error = QString::fromLocal8Bit(std::strerror(errno));
        return false;
    }
    return true;
}

QByteArray serializeStandaloneSnapshot(const StandaloneSnapshot &snapshot)
{
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("instance"), snapshot.instance);
    root.insert(QStringLiteral("applicationId"), snapshot.applicationId);
    root.insert(QStringLiteral("dropped"), QString::number(snapshot.dropped));
    QJsonArray events;
    for (const StandaloneEvent &event : snapshot.events) {
        QJsonObject item;
        item.insert(QStringLiteral("sequence"), QString::number(event.sequence));
        item.insert(QStringLiteral("time"), event.time);
        item.insert(QStringLiteral("source"), event.source);
        item.insert(QStringLiteral("message"), event.message);
        item.insert(QStringLiteral("error"), event.error);
        events.append(item);
    }
    root.insert(QStringLiteral("events"), events);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool parseStandaloneSnapshot(const QByteArray &contents, const QString &fileName,
                             StandaloneSnapshot *snapshot, QString *error)
{
    if (error) error->clear();
    QString expectedInstance;
    if (!snapshot || contents.size() > MaxStandaloneSnapshotBytes ||
        !validStandaloneFileName(fileName, &expectedInstance)) {
        if (error) *error = QStringLiteral("Invalid standalone snapshot.");
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(contents, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = QStringLiteral("Standalone snapshot is not valid JSON.");
        return false;
    }
    const QJsonObject root = document.object();
    const QString instance = root.value(QStringLiteral("instance")).toString();
    const QString applicationId = root.value(QStringLiteral("applicationId")).toString();
    bool droppedOk = false;
    const quint64 dropped = root.value(QStringLiteral("dropped")).toString().toULongLong(&droppedOk);
    const QJsonArray events = root.value(QStringLiteral("events")).toArray();
    if (root.value(QStringLiteral("version")).toInt(-1) != 1 || instance != expectedInstance ||
        !validStandaloneApplicationId(applicationId) || !droppedOk ||
        events.size() > MaxStandaloneSnapshotEvents) {
        if (error) *error = QStringLiteral("Standalone snapshot metadata is invalid.");
        return false;
    }
    static const QRegularExpression sourcePattern(QStringLiteral("\\A[A-Za-z0-9_. -]{1,128}\\z"));
    quint64 previousSequence = 0;
    StandaloneSnapshot parsed;
    parsed.fileName = fileName;
    parsed.instance = instance;
    parsed.applicationId = applicationId;
    parsed.dropped = dropped;
    parsed.events.reserve(events.size());
    for (const QJsonValue &value : events) {
        if (!value.isObject()) {
            if (error) *error = QStringLiteral("Standalone snapshot contains an invalid event.");
            return false;
        }
        const QJsonObject item = value.toObject();
        bool sequenceOk = false;
        const quint64 sequence = item.value(QStringLiteral("sequence")).toString().toULongLong(&sequenceOk);
        const QString time = item.value(QStringLiteral("time")).toString();
        const QString source = item.value(QStringLiteral("source")).toString();
        const QString message = item.value(QStringLiteral("message")).toString();
        if (!sequenceOk || sequence <= previousSequence || time.size() > 40 ||
            !QDateTime::fromString(time, Qt::ISODate).isValid() ||
            !sourcePattern.match(source).hasMatch() || message.size() > MaxStandaloneMessageChars ||
            !item.value(QStringLiteral("error")).isBool()) {
            if (error) *error = QStringLiteral("Standalone snapshot contains an invalid event.");
            return false;
        }
        previousSequence = sequence;
        parsed.events.append(StandaloneEvent{sequence, time, source, message,
                                              item.value(QStringLiteral("error")).toBool()});
    }
    *snapshot = parsed;
    return true;
}

bool collectStandaloneSnapshots(bool holdLocks, QVector<StandaloneSnapshot> *snapshots,
                                QStringList *omitted, int *directoryFd, QString *error)
{
    if (snapshots) snapshots->clear();
    if (omitted) omitted->clear();
    if (directoryFd) *directoryFd = -1;
    if (error) error->clear();
    QString path;
    {
        QMutexLocker lock(&mutex);
        path = sharedStandaloneDirectory;
        if (!sharedStandaloneError.isEmpty() && omitted)
            omitted->append(sharedStandaloneError);
    }
    if (path.isEmpty()) {
        if (omitted && omitted->isEmpty())
            omitted->append(QStringLiteral("Organization snapshots are not configured."));
        return true;
    }
    int fd = ::open(QFile::encodeName(path).constData(),
                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        if (omitted) omitted->append(QStringLiteral("Organization snapshots unavailable: %1")
                                     .arg(QString::fromLocal8Bit(std::strerror(errno))));
        return true;
    }
    QVector<QString> names;
    QString directoryError;
    if (!standaloneSnapshotNames(fd, &names, &directoryError)) {
        if (omitted) omitted->append(QStringLiteral("Cannot list organization snapshots: %1").arg(directoryError));
        ::close(fd);
        return true;
    }
    if (names.size() > MaxStandaloneSnapshots) {
        names.resize(MaxStandaloneSnapshots);
        if (omitted) omitted->append(QStringLiteral("Snapshot count limit reached; additional files were omitted."));
    }
    for (const QString &name : names) {
        QString issue;
        const int lockFd = lockStandaloneSnapshot(fd, name, &issue);
        if (lockFd == -2) {
            if (omitted) omitted->append(QStringLiteral("%1 is being updated.").arg(name));
            continue;
        }
        if (lockFd < 0) {
            if (omitted) omitted->append(QStringLiteral("%1: %2").arg(name, issue));
            continue;
        }
        QByteArray bytes;
        bool exists = false;
        struct stat fileStatus;
        StandaloneSnapshot snapshot;
        const bool readOk = readStandaloneFile(fd, name, &bytes, &exists, &issue, &fileStatus) && exists &&
            parseStandaloneSnapshot(bytes, name, &snapshot, &issue);
        if (!readOk) {
            ::flock(lockFd, LOCK_UN);
            ::close(lockFd);
            if (omitted) omitted->append(QStringLiteral("%1: %2").arg(name, issue));
            continue;
        }
        snapshot.device = fileStatus.st_dev;
        snapshot.inode = fileStatus.st_ino;
        if (holdLocks) {
            snapshot.lockFd = lockFd;
            if (directoryFd && *directoryFd < 0) *directoryFd = fd;
            else if (!directoryFd) {
                ::flock(lockFd, LOCK_UN);
                ::close(lockFd);
            }
        } else {
            ::flock(lockFd, LOCK_UN);
            ::close(lockFd);
        }
        if (snapshots) snapshots->append(snapshot);
    }
    if (holdLocks && directoryFd && *directoryFd >= 0) {
        // The caller owns the directory descriptor while it holds snapshot locks.
    } else {
        ::close(fd);
    }
    return true;
}

QString safeSnapshotText(QString text)
{
    QString safe;
    safe.reserve(text.size());
    for (const QChar character : text) {
        const ushort code = character.unicode();
        if (character == QLatin1Char('\n')) safe += QStringLiteral("\\n");
        else if (character == QLatin1Char('\r')) safe += QStringLiteral("\\r");
        else if (code < 0x20 || code == 0x7f)
            safe += QStringLiteral("\\u%1").arg(code, 4, 16, QLatin1Char('0'));
        else safe += character;
    }
    return safe;
}

QString standaloneSnapshotText(const QVector<StandaloneSnapshot> &snapshots,
                              const QStringList &omitted,
                              const QMap<QString, quint64> &exported, QSet<QString> *included,
                              bool *partial)
{
    QString text;
    if (snapshots.isEmpty() && omitted.isEmpty())
        text += QStringLiteral("\n[No standalone snapshots found in the configured organization directory; "
                               "successfully exported snapshots may already have been removed.]\n");
    if (!omitted.isEmpty()) {
        if (partial) *partial = true;
        text += QStringLiteral("\n[Standalone export partial: %1 snapshot(s) omitted or unavailable.]\n")
                    .arg(omitted.size());
        for (const QString &issue : omitted)
            text += QStringLiteral("[Standalone snapshot skipped: %1]\n").arg(safeSnapshotText(issue));
    }
    for (const StandaloneSnapshot &snapshot : snapshots) {
        text += QStringLiteral("\n[Standalone snapshot; untrusted shared file, not cryptographically authenticated; "
                               "organization apps may access it; appId=%1; instance=%2]\n")
                    .arg(snapshot.applicationId, snapshot.instance);
        if (snapshot.dropped) {
            if (partial) *partial = true;
            text += QStringLiteral("[Standalone snapshot entries dropped due to limits: %1]\n")
                        .arg(snapshot.dropped);
        }
        for (const StandaloneEvent &event : snapshot.events) {
            if (event.sequence <= exported.value(snapshot.instance, 0)) continue;
            if (included) included->insert(snapshot.instance);
            text += QStringLiteral("[%1 UTC] [%2]%3 %4\n")
                .arg(event.time, safeSnapshotText(event.source),
                     event.error ? QStringLiteral(" error") : QString(),
                     safeSnapshotText(event.message));
        }
    }
    return text;
}

QByteArray serializeWriterSnapshot(const QString &instance, const QString &applicationId,
                                   const QVector<StandaloneEvent> &events, quint64 dropped)
{
    StandaloneSnapshot snapshot;
    snapshot.instance = instance;
    snapshot.applicationId = applicationId;
    snapshot.events = events;
    snapshot.dropped = dropped;
    return serializeStandaloneSnapshot(snapshot);
}

bool flushStandaloneWriter()
{
    QString path;
    QString instance;
    QString applicationId;
    QVector<StandaloneEvent> pending;
    quint64 pendingDropped = 0;
    {
        QMutexLocker lock(&mutex);
        if (!standaloneWriter.active) return false;
        if (standaloneWriter.pending.isEmpty() && standaloneWriter.dropped == 0) return true;
        path = standaloneWriter.directory;
        instance = standaloneWriter.instance;
        applicationId = standaloneWriter.applicationId;
        pending = standaloneWriter.pending;
        pendingDropped = standaloneWriter.dropped;
    }
    if (path.isEmpty()) {
        QMutexLocker lock(&mutex);
        standaloneWriter.error = QStringLiteral("Organization snapshots are unavailable.");
        return false;
    }
    if (!validStandaloneApplicationId(applicationId)) {
        QMutexLocker lock(&mutex);
        standaloneWriter.error = QStringLiteral("Standalone application identity is invalid.");
        return false;
    }
    const QFileInfo directoryInfo(path);
    if (!directoryInfo.isDir() || directoryInfo.isSymLink() ||
        directoryInfo.canonicalFilePath() != QDir::cleanPath(path)) {
        QMutexLocker lock(&mutex);
        standaloneWriter.error = QStringLiteral("Standalone snapshot directory is not stable.");
        return false;
    }
    const int directoryFd = ::open(QFile::encodeName(path).constData(),
                                   O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directoryFd < 0) {
        QMutexLocker lock(&mutex);
        standaloneWriter.error = QString::fromLocal8Bit(std::strerror(errno));
        return false;
    }
    const QString fileName = QStringLiteral("standalone-%1.json").arg(instance);
    QString error;
    const int lockFd = lockStandaloneSnapshot(directoryFd, fileName, &error);
    if (lockFd == -2) {
        ::close(directoryFd);
        return false;
    }
    if (lockFd < 0) {
        ::close(directoryFd);
        QMutexLocker lock(&mutex);
        standaloneWriter.error = error;
        return false;
    }
    QByteArray existingBytes;
    bool exists = false;
    StandaloneSnapshot snapshot;
    bool ok = readStandaloneFile(directoryFd, fileName, &existingBytes, &exists, &error);
    if (ok && exists) {
        ok = parseStandaloneSnapshot(existingBytes, fileName, &snapshot, &error);
        if (ok && snapshot.applicationId != applicationId) {
            ok = false;
            error = QStringLiteral("Standalone snapshot application identity does not match.");
        }
        if (ok && !snapshot.events.isEmpty() && !pending.isEmpty() &&
            pending.first().sequence <= snapshot.events.last().sequence) {
            ok = false;
            error = QStringLiteral("Standalone snapshot sequence is inconsistent.");
        }
    } else if (ok) {
        snapshot.fileName = fileName;
        snapshot.instance = instance;
        snapshot.applicationId = applicationId;
        QVector<QString> names;
        if (!standaloneSnapshotNames(directoryFd, &names, &error)) {
            ok = false;
        } else if (names.size() >= MaxStandaloneSnapshots) {
            ok = false;
            error = QStringLiteral("Standalone snapshot limit reached.");
        }
    }
    if (ok) {
        snapshot.events += pending;
        const quint64 available = std::numeric_limits<quint64>::max() - snapshot.dropped;
        snapshot.dropped += qMin(available, pendingDropped);
        QByteArray contents;
        while (true) {
            contents = serializeStandaloneSnapshot(snapshot);
            if (contents.size() <= MaxStandaloneSnapshotBytes &&
                snapshot.events.size() <= MaxStandaloneSnapshotEvents) break;
            if (snapshot.events.isEmpty()) {
                ok = false;
                error = QStringLiteral("Standalone snapshot metadata exceeds its safe limits.");
                break;
            }
            dropStandaloneEvent(&snapshot.events);
            if (snapshot.dropped != std::numeric_limits<quint64>::max()) ++snapshot.dropped;
        }
        if (ok) ok = writeStandaloneFile(directoryFd, fileName, contents, &error);
    }
    ::flock(lockFd, LOCK_UN);
    ::close(lockFd);
    ::close(directoryFd);
    if (ok) {
        const quint64 lastWritten = pending.isEmpty() ? 0 : pending.last().sequence;
        QMutexLocker lock(&mutex);
        while (!standaloneWriter.pending.isEmpty() &&
               standaloneWriter.pending.first().sequence <= lastWritten)
            standaloneWriter.pending.removeFirst();
        standaloneWriter.dropped = standaloneWriter.dropped >= pendingDropped
            ? standaloneWriter.dropped - pendingDropped : 0;
        standaloneWriter.error.clear();
    } else {
        QMutexLocker lock(&mutex);
        standaloneWriter.error = error;
    }
    return ok;
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
                    exportedStandaloneSequences, rejectedRemoteEvents, storageRevision};
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
    if (ok) stream << quint32(state.standaloneExported.size());
    for (auto it = state.standaloneExported.constBegin();
         ok && it != state.standaloneExported.constEnd(); ++it) {
        ok = writeString(stream, it.key());
        if (ok) stream << it.value();
        ok = ok && stream.status() == QDataStream::Ok;
    }
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
    if (stream.status() != QDataStream::Ok || magic != SnapshotMagic ||
        (version != 1 && version != SnapshotVersion) ||
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
    if (version >= 2) {
        quint32 cursorCount = 0;
        stream >> cursorCount;
        if (stream.status() != QDataStream::Ok || cursorCount > MaxStandaloneExportCursors) {
            *error = QStringLiteral("Invalid log snapshot; the original was preserved.");
            return false;
        }
        for (quint32 i = 0; i < cursorCount; ++i) {
            QString instance;
            quint64 sequence = 0;
            if (!readString(stream, &instance, 64)) {
                *error = QStringLiteral("Invalid log snapshot; the original was preserved.");
                return false;
            }
            stream >> sequence;
            if (!validStandaloneFileName(QStringLiteral("standalone-%1.json").arg(instance), nullptr) ||
                state->standaloneExported.contains(instance) || stream.status() != QDataStream::Ok) {
                *error = QStringLiteral("Invalid log snapshot; the original was preserved.");
                return false;
            }
            state->standaloneExported.insert(instance, sequence);
        }
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
    const QString target = QDir(documentsInfo.canonicalFilePath()).filePath(QLatin1String(ExportDirectoryName));
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
    if (standaloneFilePath.isEmpty() || !ErrorLog::detailedCaptureEnabled() ||
        standaloneFileBusy.load(std::memory_order_acquire)) return;
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
    Q_UNUSED(application);
    initialized = true;
    if (!exitReportConnected && QCoreApplication::instance()) {
        QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, []() {
            ErrorLog::record(QStringLiteral("Lifecycle"), QStringLiteral("Application exited normally."), false);
        });
        exitReportConnected = true;
    }
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
    if (standaloneSnapshotMode) {
        if (!MemoryLogBus::clientDetailedEnabled()) return;
        static const QRegularExpression sourcePattern(QStringLiteral("[^A-Za-z0-9_. -]"));
        QString safeSource = source.left(128);
        safeSource.replace(sourcePattern, QStringLiteral("_"));
        if (safeSource.isEmpty()) safeSource = QStringLiteral("Unknown");
        QString boundedMessage = message;
        if (boundedMessage.size() > MaxStandaloneMessageChars)
            boundedMessage = boundedMessage.left(MaxStandaloneMessageChars - 24) +
                             QStringLiteral(" [entry truncated]");
        StandaloneEvent event{++standaloneWriter.nextSequence,
            QDateTime::currentDateTimeUtc().toString(Qt::ISODate), safeSource, boundedMessage, error};
        standaloneWriter.pending.append(event);
        int bytes = 0;
        for (const StandaloneEvent &pending : standaloneWriter.pending)
            bytes += pending.source.toUtf8().size() + pending.message.toUtf8().size() + 128;
        while (!standaloneWriter.pending.isEmpty() &&
               (bytes > MaxStandalonePendingBytes ||
                standaloneWriter.pending.size() > MaxStandaloneSnapshotEvents)) {
            const StandaloneEvent removed = dropStandaloneEvent(&standaloneWriter.pending);
            bytes -= removed.source.toUtf8().size() + removed.message.toUtf8().size() + 128;
            if (standaloneWriter.dropped != std::numeric_limits<quint64>::max())
                ++standaloneWriter.dropped;
        }
        return;
    }
    if (!detailedCaptureEnabled()) return;
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
    if (safe.isEmpty() || !initialized || !memoryDetailedEnabled) return;
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
    if (!initialized || !memoryDetailedEnabled) {
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
        memoryHistory.clear();
        recent.clear();
        memoryDetailed.clear();
        remoteHistory.clear();
        rejectedRemoteEvents = 0;
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
    QString managerText;
    QMap<QString, quint64> exported;
    {
        QMutexLocker lock(&mutex);
        if (!detailedCaptureEnabled()) {
            *error = QStringLiteral("Detailed logging is not enabled.");
            return false;
        }
        managerText = reportHeader() + MemoryLogBus::statistics() +
            QStringLiteral("\nManager log section (local and legacy remote sources):\n"
                           "Detailed log: may contain private data.\n\n") + memoryDetailed;
        if (!storageError.isEmpty())
            managerText.prepend(QStringLiteral("[Automatic log storage failed; recent events may be lost.]\n"));
        int number = 0;
        for (auto it = remoteHistory.constBegin(); it != remoteHistory.constEnd(); ++it)
            managerText += QStringLiteral("\n[Remote source %1; events are untrusted]\n").arg(++number) +
                           it.value().detailed;
        if (rejectedRemoteEvents)
            managerText += QStringLiteral("[Remote events dropped because the source limit was reached: %1]\n")
                               .arg(rejectedRemoteEvents);
        exported = exportedStandaloneSequences;
    }
    QVector<StandaloneSnapshot> snapshots;
    QStringList omitted;
    if (!collectStandaloneSnapshots(false, &snapshots, &omitted, nullptr, error)) return false;
    bool partial = !omitted.isEmpty();
    *text = managerText + standaloneSnapshotText(snapshots, omitted, exported, nullptr, &partial);
    return true;
}

bool ErrorLog::saveDetailedReport(QString *path, QString *error, bool *partial,
                                  QString *cleanupWarning)
{
    if (partial) *partial = false;
    if (cleanupWarning) cleanupWarning->clear();
    if (path) path->clear();
    if (!error) return false;
    error->clear();
    if (recording) {
        *error = QCoreApplication::translate("ErrorLog", "Log saving is already in progress.");
        return false;
    }
    RecordingGuard guard;
    QString managerText;
    QMap<QString, quint64> exported;
    {
        QMutexLocker lock(&mutex);
        if (!detailedCaptureEnabled()) {
            *error = QStringLiteral("Detailed logging is not enabled.");
            return false;
        }
        managerText = reportHeader() + MemoryLogBus::statistics() +
            QStringLiteral("\nManager log section (local and legacy remote sources):\n"
                           "Detailed log: may contain private data.\n\n") + memoryDetailed;
        if (!storageError.isEmpty())
            managerText.prepend(QStringLiteral("[Automatic log storage failed; recent events may be lost.]\n"));
        int number = 0;
        for (auto it = remoteHistory.constBegin(); it != remoteHistory.constEnd(); ++it)
            managerText += QStringLiteral("\n[Remote source %1; events are untrusted]\n").arg(++number) +
                           it.value().detailed;
        if (rejectedRemoteEvents)
            managerText += QStringLiteral("[Remote events dropped because the source limit was reached: %1]\n")
                               .arg(rejectedRemoteEvents);
        exported = exportedStandaloneSequences;
    }
    QVector<StandaloneSnapshot> snapshots;
    QStringList omitted;
    int directoryFd = -1;
    if (!collectStandaloneSnapshots(true, &snapshots, &omitted, &directoryFd, error)) return false;
    bool exportPartial = !omitted.isEmpty();
    QSet<QString> included;
    QString text = managerText + standaloneSnapshotText(snapshots, omitted, exported, &included, &exportPartial);
    for (const StandaloneSnapshot &snapshot : snapshots)
        if (snapshot.dropped) exportPartial = true;
    if (partial) *partial = exportPartial;

    if (!writeText(text, path, error)) {
        for (StandaloneSnapshot &snapshot : snapshots) {
            if (snapshot.lockFd >= 0) {
                ::flock(snapshot.lockFd, LOCK_UN);
                ::close(snapshot.lockFd);
            }
        }
        if (directoryFd >= 0) ::close(directoryFd);
        return false;
    }

    {
        QMutexLocker lock(&mutex);
        for (const StandaloneSnapshot &snapshot : snapshots) {
            if (!included.contains(snapshot.instance)) continue;
            quint64 last = exportedStandaloneSequences.value(snapshot.instance, 0);
            if (!snapshot.events.isEmpty())
                last = qMax(last, snapshot.events.last().sequence);
            exportedStandaloneSequences.insert(snapshot.instance, last);
        }
        if (!included.isEmpty()) changed();
    }
    if (!included.isEmpty() && !storagePath.isEmpty()) {
        if (storageWorker.thread.joinable()) storageWorker.thread.join();
        flushSnapshot();
        if (storageWorker.thread.joinable()) storageWorker.thread.join();
        QMutexLocker lock(&mutex);
        if (!storageError.isEmpty() && cleanupWarning)
            *cleanupWarning = QCoreApplication::translate(
                "ErrorLog", "Export succeeded, but the standalone export cursor could not be saved.");
    }

    QStringList cleanupErrors;
    QSet<QString> removedInstances;
    for (StandaloneSnapshot &snapshot : snapshots) {
        QString issue;
        if (included.contains(snapshot.instance)) {
            if (directoryFd >= 0 &&
                removeStandaloneFile(directoryFd, snapshot.fileName, &issue, &snapshot))
                removedInstances.insert(snapshot.instance);
            else
                cleanupErrors.append(QStringLiteral("%1: %2").arg(snapshot.fileName, issue));
        }
        if (snapshot.lockFd >= 0) {
            ::flock(snapshot.lockFd, LOCK_UN);
            ::close(snapshot.lockFd);
            snapshot.lockFd = -1;
        }
    }
    bool directorySynced = true;
    if (directoryFd >= 0) {
        if (::fsync(directoryFd) < 0) {
            directorySynced = false;
            cleanupErrors.append(QStringLiteral("Could not sync standalone snapshot cleanup."));
        }
        ::close(directoryFd);
    }
    if (directorySynced && !removedInstances.isEmpty()) {
        {
            QMutexLocker lock(&mutex);
            for (const QString &instance : removedInstances)
                exportedStandaloneSequences.remove(instance);
            changed();
        }
        if (!storagePath.isEmpty()) {
            if (storageWorker.thread.joinable()) storageWorker.thread.join();
            flushSnapshot();
            if (storageWorker.thread.joinable()) storageWorker.thread.join();
        }
    }
    if (!cleanupErrors.isEmpty() && cleanupWarning) {
        if (!cleanupWarning->isEmpty()) *cleanupWarning += QLatin1Char(' ');
        *cleanupWarning += QCoreApplication::translate(
            "ErrorLog", "Export succeeded, but some organization snapshots could not be removed.");
    }
    return true;
}

bool ErrorLog::clearSavedLogs(int &removed)
{
    removed = 0;
    bool ok = true;
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (documents.isEmpty()) {
        ok = false;
    } else {
        const QString root = QFileInfo(documents).canonicalFilePath();
        if (root.isEmpty()) {
            ok = false;
        } else {
            const QString target = QDir(root).filePath(QLatin1String(ExportDirectoryName));
            const int fd = ::open(QFile::encodeName(target).constData(),
                                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (fd >= 0) {
                const QStringList logs = QDir(target).entryList(
                    QStringList() << QStringLiteral("AndroidCompatibilityLayer-*.txt")
                                  << QStringLiteral("StandaloneDiagnostic-*.txt"),
                    QDir::Files | QDir::NoSymLinks);
                for (const QString &log : logs) {
                    const QByteArray name = QFile::encodeName(log);
                    struct stat status;
                    if (::fstatat(fd, name.constData(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
                        if (errno != ENOENT) ok = false;
                    } else if (S_ISREG(status.st_mode)) {
                        if (::unlinkat(fd, name.constData(), 0) == 0) ++removed;
                        else if (errno != ENOENT) ok = false;
                    }
                }
                ::close(fd);
            } else if (errno != ENOENT) {
                ok = false;
            }
        }
    }

    QString sharedPath;
    {
        QMutexLocker lock(&mutex);
        sharedPath = sharedStandaloneDirectory;
    }
    if (!sharedPath.isEmpty()) {
        const int sharedFd = ::open(QFile::encodeName(sharedPath).constData(),
                                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (sharedFd >= 0) {
            QVector<QString> snapshots;
            QString listError;
            if (!standaloneSnapshotNames(sharedFd, &snapshots, &listError)) ok = false;
            if (snapshots.size() > MaxStandaloneSnapshots) {
                snapshots.resize(MaxStandaloneSnapshots);
                ok = false;
            }
            for (const QString &name : snapshots) {
                QString issue;
                const int lockFd = lockStandaloneSnapshot(sharedFd, name, &issue);
                if (lockFd < 0) {
                    ok = false;
                    continue;
                }
                if (removeStandaloneFile(sharedFd, name, &issue)) ++removed;
                else ok = false;
                ::flock(lockFd, LOCK_UN);
                ::close(lockFd);
            }
            if (::fsync(sharedFd) < 0) ok = false;
            ::close(sharedFd);
        } else if (errno != ENOENT) {
            ok = false;
        }
    }
    return ok;
}

bool ErrorLog::startCollector(QString *error)
{
    const bool started = MemoryLogBus::startServer(
        [](const QString &identity, const QString &source, const QString &message, bool isError, quint64 dropped) {
            QMutexLocker lock(&mutex);
            if (!memoryDetailedEnabled) return;
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
                appendBounded(&history.detailed, note, MaxRemoteDetailed, DetailedTruncated);
            }
            if (message.isEmpty()) return;
            const QString safe = safeEvent(source, message, isError);
            if (!safe.isEmpty())
                appendBounded(&history.safe, QStringLiteral("[%1 UTC] %2\n").arg(time, safe),
                              MaxRemoteSafe, JournalTruncated);
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
    bool sendAppend = true;
    {
        QMutexLocker lock(&mutex);
        sendAppend = !standaloneSnapshotMode;
    }
    if (!MemoryLogBus::startClient(error, sendAppend)) return false;
    return true;
}

bool ErrorLog::setStandaloneSnapshotRoot(const QString &sharedRoot, QString *error)
{
    if (error) error->clear();
    QString path;
    int fd = -1;
    QString issue;
    if (!openStandaloneDirectory(sharedRoot, true, &path, &fd, &issue)) {
        QMutexLocker lock(&mutex);
        sharedStandaloneDirectory.clear();
        sharedStandaloneError = issue;
        if (error) *error = issue;
        return false;
    }
    ::close(fd);
    {
        QMutexLocker lock(&mutex);
        sharedStandaloneDirectory = path;
        sharedStandaloneError.clear();
    }
    return true;
}

bool ErrorLog::startStandaloneSnapshots(const QString &sharedRoot, const QString &applicationId,
                                        QString *error)
{
    if (!error) return false;
    error->clear();
    {
        QMutexLocker lock(&mutex);
        if (standaloneWriter.active) {
            *error = QStringLiteral("Standalone snapshot service is already running.");
            return false;
        }
    }
    QString issue;
    const bool rootReady = setStandaloneSnapshotRoot(sharedRoot, &issue);
    const bool validApplication = validStandaloneApplicationId(applicationId);
    {
        QMutexLocker lock(&mutex);
        standaloneSnapshotMode = true;
        standaloneWriter.active = true;
        standaloneWriter.directory = rootReady ? sharedStandaloneDirectory : QString();
        standaloneWriter.instance = newStandaloneInstanceId();
        standaloneWriter.applicationId = applicationId;
        standaloneWriter.error = rootReady ? QString() : issue;
        if (!validApplication)
            standaloneWriter.error = QStringLiteral("Standalone application identity is invalid.");
    }
    if (!rootReady || !validApplication) {
        *error = standaloneSnapshotError();
        return false;
    }
    if (!QCoreApplication::instance()) {
        *error = QStringLiteral("Standalone snapshot service is unavailable.");
        QMutexLocker lock(&mutex);
        standaloneWriter.error = *error;
        return false;
    }
    QTimer *timer = new QTimer(QCoreApplication::instance());
    timer->setInterval(500);
    QObject::connect(timer, &QTimer::timeout, timer, []() { ErrorLog::flushStandaloneSnapshots(); });
    QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, []() {
        ErrorLog::flushStandaloneSnapshots();
    });
    timer->start();
    return true;
}

bool ErrorLog::flushStandaloneSnapshots()
{
    return flushStandaloneWriter();
}

QString ErrorLog::standaloneSnapshotError()
{
    QMutexLocker lock(&mutex);
    return standaloneWriter.error.isEmpty() ? sharedStandaloneError : standaloneWriter.error;
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
        memoryDetailedEnabled = old.detailedEnabled;
        if (old.detailedEnabled) {
            memoryHistory = old.history;
            memoryDetailed = old.detailed;
            remoteHistory = old.remote;
            rejectedRemoteEvents = old.rejected;
        }
        exportedStandaloneSequences = old.standaloneExported;
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
    } else {
        if (!writeText(reportHeader(), path, error, true)) {
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
