#include "error_log.h"
#include "memory_log_bus.h"
#include "package_architecture.h"

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
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <poll.h>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
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
QStringList overlayLines;
QMap<QString, QString> overlayFragments;
struct OverlayRule {
    const char *feature;
    const char *pattern;
    const char *display;
};
const OverlayRule overlayRules[] = {
    {"camera-still", "camera: Rockchip front stills use viewfinder JPEG", "Camera: front viewfinder still enabled"},
    {"camera-still", "camera: viewfinder still JPEG captured", "Camera: front JPEG captured"},
    {"camera-still", "camera: viewfinder still could not be encoded", "Camera: front JPEG failed"},
    {"camera-still", "camera: Rockchip front viewfinder JPEG unavailable", "Camera: front JPEG unavailable"},
    {"system-ca", "ACL CA: system certificate store ready", "CA: system certificates ready"},
    {"system-ca", "ACL CA: system bundle unavailable", "CA: built-in certificates only"},
    {"system-ca", "ACL CA: system certificate store failed", "CA: system certificate store failed"},
    {"codecs", "ACL codec: rockchip mpp library unavailable", "Codec HW: rockchip mpp library unavailable"},
    {"codecs", "ACL codec: rockchip mpp ABI unavailable", "Codec HW: rockchip mpp ABI unavailable"},
    {"codecs", "ACL codec: rockchip mpp capability rejected", "Codec HW: rockchip mpp capability rejected"},
    {"codecs", "ACL codec: rockchip mpp decoder initialized", "Codec HW: rockchip mpp decoder initialized"},
    {"codecs", "ACL codec: rockchip mpp first picture", "Codec HW: rockchip mpp first picture"},
    {"codecs", "ACL codec: rockchip mpp initialization failed", "Codec HW: rockchip mpp initialization failed"},
    {"codecs", "ACL codec: rockchip mpp output timeout setup failed", "Codec HW: rockchip mpp output timeout setup failed"},
    {"codecs", "ACL codec: rockchip mpp output frame rejected", "Codec HW: rockchip mpp output frame rejected"},
    {"codecs", "ACL codec: rockchip mpp buffer synchronization failed", "Codec HW: rockchip mpp buffer synchronization failed"},
    {"codecs", "ACL codec: rockchip mpp dequeue failed", "Codec HW: rockchip mpp dequeue failed"},
    {"codecs", "ACL codec: rockchip mpp info change failed", "Codec HW: rockchip mpp info change failed"},
    {"codecs", "ACL codec: rockchip mpp output frame error", "Codec HW: rockchip mpp output frame error"},
    {"codecs", "ACL codec: rockchip mpp frame release failed", "Codec HW: rockchip mpp frame release failed"},
    {"codecs", "ACL codec: rockchip mpp packet allocation failed", "Codec HW: rockchip mpp packet allocation failed"},
    {"codecs", "ACL codec: rockchip mpp queue failed", "Codec HW: rockchip mpp queue failed"},
    {"codecs", "ACL codec: rockchip mpp drain timeout", "Codec HW: rockchip mpp drain timeout"},
    {"codecs", "ACL codec: rockchip mpp drain produced no pictures", "Codec HW: rockchip mpp drain produced no pictures"},
    {"codecs", "ACL codec: rockchip mpp reset failed", "Codec HW: rockchip mpp reset failed"},
    {"codecs", "ACL codec: hardware replay limit reached", "Codec HW: replay limit reached"},
    {"codecs", "ACL codec: droidmedia hardware fallback active", "Codec HW: droidmedia fallback active"},
    {"codecs", "ACL codec: droidmedia decoder or FFmpeg 8 unavailable", "Codec HW: droidmedia decoder or FFmpeg 8 unavailable"},
    {"codecs", "ACL codec: droidmedia library or ABI unavailable", "Codec HW: droidmedia library or ABI unavailable"},
    {"codecs", "ACL codec: droidmedia capability check rejected", "Codec HW: droidmedia capability check rejected"},
    {"codecs", "ACL codec: droidmedia create or configure failed", "Codec HW: droidmedia create or configure failed"},
    {"codecs", "ACL codec: droidmedia start failed", "Codec HW: droidmedia start failed"},
    {"codecs", "ACL codec: droidmedia decoder initialization failed", "Codec HW: droidmedia initialization failed"},
    {"codecs", "ACL codec: droidmedia queue timeout", "Codec HW: droidmedia queue timeout"},
    {"codecs", "ACL codec: droidmedia queue wait failed", "Codec HW: droidmedia queue wait failed"},
    {"codecs", "ACL codec: droidmedia queue failed", "Codec HW: droidmedia queue failed"},
    {"codecs", "ACL codec: droidmedia drain timeout", "Codec HW: droidmedia drain timeout"},
    {"codecs", "ACL codec: droidmedia drain wait failed", "Codec HW: droidmedia drain wait failed"},
    {"codecs", "ACL codec: droidmedia drain failed", "Codec HW: droidmedia drain failed"},
    {"codecs", "ACL codec: droidmedia dequeue failed", "Codec HW: droidmedia dequeue failed"},
    {"codecs", "Unsupported droidmedia output color format", "Codec HW: droidmedia unsupported output format"},
    {"codecs", "ACL codec: droidmedia output frame rejected", "Codec HW: droidmedia output frame rejected"},
    {"codecs", "ACL codec: droidmedia asynchronous error", "Codec HW: droidmedia asynchronous decoder error"},
    {"codecs", "ACL codec: droidmedia replay limit reached", "Codec HW: droidmedia replay limit reached"},
    {"codecs", "ACL codec: AAC FFmpeg 62 initialization failed", "Codec AAC: FFmpeg 8 initialization failed"},
    {"codecs", "ACL codec: AAC FFmpeg 61 initialization failed", "Codec AAC: FFmpeg 7 initialization failed"},
    {"codecs", "ACL codec: video FFmpeg 62 initialization failed", "Codec video: FFmpeg 8 initialization failed"},
    {"codecs", "ACL codec: video FFmpeg 61 initialization failed", "Codec video: FFmpeg 7 initialization failed"},
    {"codecs", "AndroidCompatibilityLayer audio codec: decoder initialization failed", "Codec: decoder initialization failed"},
    {"codecs", "droidmedia disabled after a stop failure", "Codec HW: droidmedia disabled after stop failure"},
    {"codecs", "cannot start bounded droidmedia stop", "Codec HW: droidmedia stop worker unavailable"},
    {"droidmedia", "droidmedia started:", "droidmedia started"},
    {"droidmedia", "droidmedia stop timed out; disabling hardware decoder for this process", "droidmedia stop timed out; disabling hardware decoder for this process"},
    {"droidmedia", "droidmedia.*(?:fail|error|timeout|timed out|fallback)", "droidmedia: error, timeout or fallback"},
    {"codecs", "ACL codec: first hardware picture", "ACL codec: first hardware picture"},
    {"codecs", "ACL codec: video software decoder selected", "ACL codec: video software decoder selected"},
    {"codecs", "ACL codec: video software fallback active", "ACL codec: video software fallback active"},
    {"codecs", "media: extractor: 0 compressed tracks", "Media extractor: no compressed tracks"},
    {"codecs", "media: extractor: [1-9][0-9]* compressed tracks", "Media extractor: compressed tracks available"},
    {"codecs", "(?:codec|decoder|encoder).*(?:(?<!\\b0 )fail|error|timeout|timed out|fallback|unavailable)|(?:fallback|timeout).*(?:codec|decoder|encoder)", "codec: error, timeout or fallback"},
    {"gpu-canvas", "Experimental GPU Canvas active", "GPU Canvas active"},
    {"gpu-canvas", "Experimental GPU presentation active; Qt consumed an EGLImage", "GPU presentation active: Qt consumed an EGLImage"},
    {"gpu-canvas", "Experimental GPU Canvas unavailable; using the CPU Canvas fallback", "GPU Canvas unavailable: CPU fallback"},
    {"gpu-canvas", "GPU Canvas falling back to CPU: could not create a complete color-filter framebuffer", "GPU Canvas CPU fallback: color-filter framebuffer"},
    {"gpu-canvas", "GPU Canvas falling back to CPU: could not create a complete framebuffer", "GPU Canvas CPU fallback: framebuffer"},
    {"gpu-canvas", "GPU Canvas falling back to CPU: shared OpenGL context is unavailable", "GPU Canvas CPU fallback: shared context unavailable"},
    {"gpu-canvas", "GPU Canvas falling back to CPU: bitmap exceeds the GPU's maximum texture size", "GPU Canvas CPU fallback: texture too large"},
    {"gpu-canvas", "GPU Canvas falling back to CPU: GPU bitmap allocation or upload failed", "GPU Canvas CPU fallback: bitmap allocation or upload"},
    {"gpu-canvas", "Experimental GPU Canvas presentation failed", "GPU Canvas presentation failed: CPU fallback"},
    {"gpu-canvas", "GPU Canvas.*(?:capture|import).*native EGL texture", "GPU Canvas: native EGL texture capture/import error"},
    {"gpu-canvas", "GPU Canvas EGL.*(?:fail|cannot|unavailable)", "GPU Canvas: EGL capability error"},
    {"gpu-canvas", "java:.*(?:no EGL|no OpenGL ES .*context|surface could be made|context was lost)", "Android EGL: context or surface error"},
    {"gpu-canvas", "java: the shell did not take an OpenGL ES frame", "Android GL: CPU composition fallback"},
    {"google-login-gpu", "WebView GPU: native EGL texture capture ready", "WebView GPU: capture ready"},
    {"google-login-gpu", "WebView GPU: native EGL texture capture unavailable; software fallback", "WebView GPU: software fallback"},
    {"google-login-frames", "Shared microG UI frame ready:", "Shared microG UI frame ready"},
    {"google-login-frames", "Shared microG frame suppressed: window ownership is unavailable", "Shared microG frame suppressed: window ownership is unavailable"},
    {"google-login-frames", "Shared microG UI cleanup is blocked; sign-in remains reserved", "Shared microG UI cleanup is blocked; sign-in remains reserved"},
    {"ordered-broadcasts", "Shared microG client ordered broadcast received", "Shared microG client ordered broadcast received"},
    {"ordered-broadcasts", "ordered broadcast.*(?:fail|error|closed|lost|reject|invalid|unavailable|pending|could not)", "microG: ordered broadcast error"},
    {"microg-bridge", "Binder bridge disconnected:", "Binder bridge disconnected"},
    {"microg-bridge", "Shared microG provider stream transfer failed:", "Shared microG provider stream transfer failed"},
    {"microg-fd", "Shared microG Binder descriptors sent", "Shared microG Binder descriptors sent"},
    {"microg-fd", "Shared microG Binder descriptors received", "Shared microG Binder descriptors received"},
    {"microg-fd", "(?:microg|binder|parcel|bridge).*(?:\\bfd\\b|file descriptor)|(?:\\bfd\\b|file descriptor).*(?:microg|binder|parcel|bridge)", "microG: file descriptor / bridge"},
    {"google-login", "Shared microG client account result: success", "Shared microG client account result: success"},
    {"google-login", "Shared microG client account result: cancelled", "Shared microG client account result: cancelled"},
    {"google-login", "Shared microG client account result: failed", "Shared microG client account result: failed"},
    {"google-login", "Shared microG client channel ready\\.", "Google / microG: client channel ready"},
    {"google-login", "Shared microG client channel open failed\\.", "Google / microG: client channel open failed"},
    {"microg-tls-module", "Shared microG module APK ready\\.", "microG module: APK ready"},
    {"microg-tls-module", "Shared microG module APK unavailable:", "microG module: APK unavailable"},
    {"google-login", "GmsClient: unable to connect to service:", "Google / microG: GMS service connection failed"},
    {"google-login", "ServiceBindIntentUtils: Dynamic intent resolution failed:", "Google / microG: dynamic service resolution failed"},
    {"google-login", "DynamiteModule: Invalid GmsCore APK, remote loading disabled\\.", "Google / microG: GMS module package unavailable"},
    {"google-login", "Shared microG service bind failed\\.|Shared microG rejected the bind transaction\\.", "Google / microG: bind failed"},
    {"google-login", "Shared microG service binding refused: service unavailable or not public", "Google / microG: service unavailable or private"},
    {"google-login", "Shared Google account service unavailable|Shared account service unavailable|Shared microG client bootstrap is unavailable", "Google / microG: account bridge unavailable"},
    {"google-login", "The shared microG worker is unavailable\\.|Shared microG backend unavailable:", "Google / microG: worker unavailable"},
    {"google-login", "Shared microG client startup is unavailable\\.|Shared microG descriptor is unavailable\\.", "Google / microG: client startup unavailable"},
    {"google-login", "Shared microG client router is closed\\.|Shared microG client channel is closed\\.|Account channel closed", "Google / microG: client channel closed"},
    {"google-login", "Google account is unavailable", "Google / microG: requested account unavailable"},
    {"google-login", "Another Google sign-in is active or the account screen is unavailable", "Google / microG: sign-in busy or screen unavailable"},
    {"google-login", "Shared microG account screen launch failed|starting shared microG activity failed", "Google / microG: sign-in screen launch failed"},
    {"google-login", "Account request timed out|Shared microG account outcome: operation=[235] outcome=TIMEOUT\\b", "Google / microG: account request timeout"},
    {"google-login", "Shared microG account outcome: operation=3 outcome=ERROR\\b", "Google / microG: auth token request failed"},
    {"google-login", "Shared microG account outcome: operation=2 outcome=ERROR\\b", "Google / microG: add account request failed"},
    {"google-login", "Shared microG client account callback failed", "Google / microG: account callback failed"},
    {"google-login", "checkin: Google Play services has no Android ID after [0-9]+ ms", "Google / microG: check-in timed out"},
    {"notifications", "System notification service is unavailable", "Android notification: D-Bus unavailable"},
    {"notifications", "System notification permission denied", "Android notification: permission denied"},
    {"notifications", "System notification post failed", "Android notification: post failed"},
    {"notifications", "System notification update failed", "Android notification: update failed"},
    {"notifications", "System notification cancellation failed\\.|notification cancellation failed", "Android notification: cancel failed"},
    {"notifications", "notification delivery failed", "Android notification: post/update failed"},
    {"notifications", "Android notification limit reached\\.", "Android notification: limit reached"},
    {"notifications", "ACL notification: posted", "Android notification: posted"},
    {"notifications", "ACL notification: removed", "Android notification: removed"},
    {"background", "ACL background: job started", "Background job: started"},
    {"background", "ACL background: job finished", "Background job: finished"},
    {"background", "ACL background: job stopped", "Background job: cancelled by app"},
    {"background", "ACL background: job failed", "Background job: failed"},
    {"background", "ACL background: WorkManager job started", "WorkManager job: started"},
    {"background", "ACL background: WorkManager job finished", "WorkManager job: finished"},
    {"background", "ACL background: WorkManager job stopped", "WorkManager job: finished"},
    {"background", "ACL background: alarm scheduled", "Alarm: scheduled"},
    {"background", "ACL background: alarm fired", "Alarm: fired"},
    {"background", "ACL background: alarm failed", "Alarm: failed"},
    {"background", "ACL background: foreground service started", "Foreground service: started"},
    {"background", "ACL background: foreground service stopped", "Foreground service: finished"},
    {"crashes-and-bridges", "fatal|crash|sigsegv|sigabrt|segmentation fault|uncaught exception|(?:bridge|microg).*(?:fail|error|timeout|timed out|disconnect)", "Crash or bridge error"}
};
void appendOverlayLine(const QString &line)
{
    const QString trimmed = line.left(4096).trimmed();
    if (trimmed.isEmpty()) return;
    static const QVector<QRegularExpression> patterns = []() {
        QVector<QRegularExpression> result;
        for (const OverlayRule &rule : overlayRules)
            result.append(QRegularExpression(QString::fromLatin1(rule.pattern),
                                             QRegularExpression::CaseInsensitiveOption));
        return result;
    }();
    for (int i = 0; i < patterns.size(); ++i) {
        if (!patterns.at(i).match(trimmed).hasMatch()) continue;
        overlayLines.append(QString::fromLatin1(overlayRules[i].display));
        while (overlayLines.size() > 6) overlayLines.removeFirst();
        return;
    }
}

void appendOverlay(const QString &source, const QString &message)
{
    const bool stream = source.endsWith(QLatin1String("stdout")) ||
                        source.endsWith(QLatin1String("stderr"));
    QString text = stream ? overlayFragments.take(source) + message : message;
    const QStringList lines = text.split(QLatin1Char('\n'));
    const int complete = stream && !text.endsWith(QLatin1Char('\n')) ? lines.size() - 1 : lines.size();
    for (int i = 0; i < complete; ++i) appendOverlayLine(lines.at(i));
    if (complete < lines.size()) {
        if (!overlayFragments.contains(source) && overlayFragments.size() >= 32)
            overlayFragments.erase(overlayFragments.begin());
        overlayFragments.insert(source, lines.last().right(4096));
    }
}
std::atomic<bool> memoryDetailedEnabled(false);
std::atomic<bool> memoryDetailedPaused(false);
bool initialized = false;
struct RemoteHistory {
    QString safe;
    QString detailed;
};
QMap<QString, RemoteHistory> remoteHistory;
quint64 rejectedRemoteEvents = 0;
const int MaxRemoteClients = 32;
const int MaxRemoteSafe = 128 * 1024;
const int MaxRemoteDetailed = 10000000;
const int MaxContext = 131072;
const int MaxJournalBytes = 4 * 1024 * 1024;
const int MaxJournalEntry = 10000000;
const int MaxDetailedBytes = 20000000;
const int MaxReportBytes = 10000000;
const char ExportDirectoryName[] = "AndroidCompatibilityLayer-Error-Logs";
const char SharedStandaloneDirectoryName[] = "AndroidCompatibilityLayer-Standalone-Logs";
const char JournalTruncated[] = "[Earlier shared history was truncated.]\n";
const char DetailedTruncated[] = "[Earlier detailed output was truncated.]\n";
const int MaxStandaloneSnapshots = 32;
const int MaxStandaloneSnapshotBytes = 64 * 1024 * 1024;
const int MaxStandaloneSnapshotEvents = 500000;
const int StandaloneStartupEvents = 32;
const int MaxStandalonePendingBytes = MaxReportBytes;
const int MaxStandaloneMessageChars = MaxReportBytes;
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
StorageWorker standaloneSnapshotWorker;
std::atomic<bool> standaloneSnapshotBusy(false);
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
    int pendingBytes = 0;
    quint64 nextSequence = 0;
    quint64 dropped = 0;
    QString lastErrorSource;
    QString lastErrorSafeSource;
    QString lastErrorMessage;
    QString lastRepeatTime;
    quint64 repeats = 0;
};
StandaloneWriter standaloneWriter;
QString sharedStandaloneDirectory;
QString sharedStandaloneError;
bool standaloneSnapshotMode = false;
QMap<QString, quint64> exportedStandaloneSequences;
int coreSender = -1;
int coreReceiver = -1;
static_assert(ATOMIC_INT_LOCK_FREE == 2, "Core log atomics must be lock-free");
std::atomic<unsigned int> coreDropped(0);
std::atomic<unsigned int> coreIncomplete(0);
const size_t MaxCoreQueueBytes = 10000000;
struct CorePacket {
    quint32 id;
    quint32 offset;
    quint32 total;
    int level;
    quint32 abort;
};
struct CoreMessage {
    int level;
    QByteArray text;
};
struct CoreChannel {
    int wakeSender = -1;
    int wakeReceiver = -1;
    std::mutex mutex;
    std::deque<CoreMessage> pending;
    size_t bytes = 0;
    std::atomic<bool> stop{false};
    std::atomic<unsigned int> nextId{0};
    std::thread worker;
    ~CoreChannel()
    {
        stop.store(true, std::memory_order_relaxed);
        if (worker.joinable()) worker.join();
        if (wakeSender >= 0) ::close(wakeSender);
        if (wakeReceiver >= 0) ::close(wakeReceiver);
        if (coreSender >= 0) ::close(coreSender);
        if (coreReceiver >= 0) ::close(coreReceiver);
    }
} coreChannel;
void finishCoreChannel();
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
    const QString architecture = packageBuildArchitecture();
    return QStringLiteral("Report format: 2\nBuild: %1 %2\n"
                          "System: %3\nPackage architecture (build): %4\nQt: %5\nTime: %6 UTC\n\n")
        .arg(QString::fromLatin1(__DATE__)).arg(QString::fromLatin1(__TIME__))
        .arg(versionPattern.match(version).hasMatch() ? system + QLatin1Char(' ') + version : system)
        .arg(architecture.isEmpty() ? QStringLiteral("unknown") : architecture)
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
        "|Standalone RPM: pid=[0-9]{1,20} request=[0-9]{1,20} event=(?:"
        "accepted|ignored-busy|preflight-failed|failed-to-start|timeout|stage-(?:inspect|build|sign|save)"
        "|started worker=[0-9]{1,20}|finished exit=-?[0-9]{1,10} normal=[01] timed-out=[01])"
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
    static const QRegularExpression pattern(QStringLiteral("\\Astandalone-([0-9a-f]{32})\\.log\\z"));
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

QString safeSnapshotText(QString text);

QString standaloneEventText(const StandaloneEvent &event)
{
    return QStringLiteral("[%1 UTC] [%2]%3 %4\n")
        .arg(event.time, safeSnapshotText(event.source),
             event.error ? QStringLiteral(" error") : QString(),
             safeSnapshotText(event.message));
}

int standalonePendingEventBytes(const StandaloneEvent &event)
{
    return standaloneEventText(event).toUtf8().size();
}

void appendStandalonePending(const QString &time, const QString &source,
                             const QString &message, bool error)
{
    StandaloneEvent event{++standaloneWriter.nextSequence, time, source, message, error};
    standaloneWriter.pending.append(event);
    standaloneWriter.pendingBytes += standalonePendingEventBytes(event);
    while (!standaloneWriter.pending.isEmpty() &&
           (standaloneWriter.pendingBytes > MaxStandalonePendingBytes ||
            standaloneWriter.pending.size() > MaxStandaloneSnapshotEvents)) {
        const StandaloneEvent removed = dropStandaloneEvent(&standaloneWriter.pending);
        standaloneWriter.pendingBytes -= standalonePendingEventBytes(removed);
        if (standaloneWriter.dropped != std::numeric_limits<quint64>::max())
            ++standaloneWriter.dropped;
    }
}

void appendStandaloneRepeatSummary()
{
    if (!standaloneWriter.repeats) return;
    const QString prefix = QStringLiteral("[Repeated %1 additional times; last %2 UTC] ")
        .arg(standaloneWriter.repeats).arg(standaloneWriter.lastRepeatTime);
    const QString message = prefix + standaloneWriter.lastErrorMessage.left(
        MaxStandaloneMessageChars - prefix.size());
    appendStandalonePending(standaloneWriter.lastRepeatTime, standaloneWriter.lastErrorSafeSource,
                            message, true);
    standaloneWriter.repeats = 0;
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

QByteArray escapeStandaloneField(const QString &text)
{
    QByteArray bytes = text.toUtf8();
    QByteArray escaped;
    escaped.reserve(bytes.size());
    for (const char character : bytes) {
        if (character == '\\') escaped += "\\\\";
        else if (character == '\n') escaped += "\\n";
        else if (character == '\r') escaped += "\\r";
        else if (character == '\t') escaped += "\\t";
        else escaped += character;
    }
    return escaped;
}

QString unescapeStandaloneField(const QByteArray &bytes)
{
    QByteArray plain;
    plain.reserve(bytes.size());
    for (int i = 0; i < bytes.size(); ++i) {
        const char character = bytes.at(i);
        if (character != '\\' || i + 1 >= bytes.size()) {
            plain += character;
            continue;
        }
        const char next = bytes.at(++i);
        plain += next == 'n' ? '\n' : next == 'r' ? '\r' : next == 't' ? '\t' : next;
    }
    return QString::fromUtf8(plain);
}

// Append-only standalone log: one header line, then one line per event ("E") or drop count ("D").
QByteArray standaloneHeaderLine(const QString &instance, const QString &applicationId)
{
    return "ACL-STANDALONE-LOG\t2\t" + instance.toUtf8() + '\t' + applicationId.toUtf8() + '\n';
}

QByteArray standaloneEventLine(const StandaloneEvent &event)
{
    return "E\t" + QByteArray::number(event.sequence) + '\t' + event.time.toUtf8() + '\t' +
           event.source.toUtf8() + '\t' + (event.error ? '1' : '0') + '\t' +
           escapeStandaloneField(event.message) + '\n';
}

QByteArray standaloneDroppedLine(quint64 dropped)
{
    return "D\t" + QByteArray::number(dropped) + '\n';
}

QByteArray serializeStandaloneSnapshot(const StandaloneSnapshot &snapshot)
{
    QByteArray contents = standaloneHeaderLine(snapshot.instance, snapshot.applicationId);
    if (snapshot.dropped) contents += standaloneDroppedLine(snapshot.dropped);
    for (const StandaloneEvent &event : snapshot.events) contents += standaloneEventLine(event);
    return contents;
}

bool parseStandaloneHeader(const QByteArray &line, const QString &expectedInstance, QString *applicationId)
{
    const QList<QByteArray> fields = line.split('\t');
    if (fields.size() != 4 || fields.at(0) != "ACL-STANDALONE-LOG" || fields.at(1) != "2" ||
        QString::fromUtf8(fields.at(2)) != expectedInstance)
        return false;
    *applicationId = QString::fromUtf8(fields.at(3));
    return validStandaloneApplicationId(*applicationId);
}

// Parses complete lines from start; returns the offset after the last complete line.
// An unfinished trailing line is left for the next read.
int parseStandaloneLines(const QByteArray &bytes, int start, quint64 *lastSequence,
                         QVector<StandaloneEvent> *events, quint64 *dropped)
{
    static const QRegularExpression sourcePattern(QStringLiteral("\\A[A-Za-z0-9_. -]{1,128}\\z"));
    int offset = start;
    while (offset < bytes.size()) {
        const int end = bytes.indexOf('\n', offset);
        if (end < 0) break;
        const QByteArray line = bytes.mid(offset, end - offset);
        offset = end + 1;
        const QList<QByteArray> fields = line.split('\t');
        if (fields.size() == 2 && fields.at(0) == "D") {
            bool ok = false;
            const quint64 count = fields.at(1).toULongLong(&ok);
            if (ok) *dropped += qMin(count, std::numeric_limits<quint64>::max() - *dropped);
            continue;
        }
        bool sequenceOk = false;
        const quint64 sequence = fields.size() == 6 ? fields.at(1).toULongLong(&sequenceOk) : 0;
        const QString time = sequenceOk ? QString::fromUtf8(fields.at(2)) : QString();
        const QString source = sequenceOk ? QString::fromUtf8(fields.at(3)) : QString();
        if (fields.at(0) != "E" || !sequenceOk || sequence <= *lastSequence || time.size() > 40 ||
            !QDateTime::fromString(time, Qt::ISODate).isValid() ||
            !sourcePattern.match(source).hasMatch() || (fields.at(4) != "0" && fields.at(4) != "1")) {
            if (*dropped != std::numeric_limits<quint64>::max()) ++*dropped;
            continue;
        }
        *lastSequence = sequence;
        QString message = unescapeStandaloneField(fields.at(5));
        if (message.size() > MaxStandaloneMessageChars) message.truncate(MaxStandaloneMessageChars);
        events->append(StandaloneEvent{sequence, time, source, message, fields.at(4) == "1"});
        if (events->size() > MaxStandaloneSnapshotEvents) {
            events->removeFirst();
            if (*dropped != std::numeric_limits<quint64>::max()) ++*dropped;
        }
    }
    return offset;
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
    const int headerEnd = contents.indexOf('\n');
    StandaloneSnapshot parsed;
    if (headerEnd < 0 ||
        !parseStandaloneHeader(contents.left(headerEnd), expectedInstance, &parsed.applicationId)) {
        if (error) *error = QStringLiteral("Standalone snapshot metadata is invalid.");
        return false;
    }
    parsed.fileName = fileName;
    parsed.instance = expectedInstance;
    quint64 lastSequence = 0;
    parseStandaloneLines(contents, headerEnd + 1, &lastSequence, &parsed.events, &parsed.dropped);
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
    qint64 collectedBytes = 0;
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
        bool readOk = readStandaloneFile(fd, name, &bytes, &exists, &issue, &fileStatus) && exists;
        if (readOk && bytes.size() > MaxStandaloneSnapshotBytes - collectedBytes) {
            readOk = false;
            issue = QStringLiteral("Snapshot collection size limit reached; file was retained.");
        }
        if (readOk) {
            readOk = parseStandaloneSnapshot(bytes, name, &snapshot, &issue);
            if (readOk) collectedBytes += bytes.size();
        }
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
                              const QMap<QString, quint64> &exported,
                              QMap<QString, quint64> *exportedThrough,
                              QSet<QString> *completed, bool *partial, int byteBudget,
                              bool *sizeExceeded)
{
    QString text;
    if (sizeExceeded) *sizeExceeded = false;
    int bytes = 0;
    const auto append = [&](const QString &item) {
        const int size = item.toUtf8().size();
        if (size > byteBudget - bytes) return false;
        text += item;
        bytes += size;
        return true;
    };
    const auto truncated = [&]() {
        if (partial) *partial = true;
        if (sizeExceeded) *sizeExceeded = true;
        return text;
    };
    if (snapshots.isEmpty() && omitted.isEmpty() &&
        !append(QStringLiteral("\n[No standalone snapshots found in the configured organization directory; "
                               "successfully exported snapshots may already have been removed.]\n"))) {
        return truncated();
    }
    if (!omitted.isEmpty()) {
        if (partial) *partial = true;
        if (!append(QStringLiteral("\n[Standalone export partial: %1 snapshot(s) omitted or unavailable.]\n")
                    .arg(omitted.size())))
            return truncated();
        for (const QString &issue : omitted)
            if (!append(QStringLiteral("[Standalone snapshot skipped: %1]\n").arg(safeSnapshotText(issue))))
                return truncated();
    }
    for (const StandaloneSnapshot &snapshot : snapshots) {
        if (!append(QStringLiteral("\n[Standalone snapshot; untrusted shared file, not cryptographically authenticated; "
                               "organization apps may access it; appId=%1; instance=%2]\n")
                    .arg(snapshot.applicationId, snapshot.instance))) {
            return truncated();
        }
        if (snapshot.dropped) {
            if (partial) *partial = true;
            if (!append(QStringLiteral("[Standalone snapshot entries dropped due to limits: %1]\n")
                        .arg(snapshot.dropped)))
                return truncated();
        }
        bool hadEvents = false;
        for (const StandaloneEvent &event : snapshot.events) {
            if (event.sequence <= exported.value(snapshot.instance, 0)) continue;
            if (!append(standaloneEventText(event))) {
                return truncated();
            }
            hadEvents = true;
            if (exportedThrough) exportedThrough->insert(snapshot.instance, event.sequence);
        }
        if (hadEvents && completed) completed->insert(snapshot.instance);
    }
    return text;
}

QString boundedReportManagerText(const QString &text, int managerLimit, bool *partial)
{
    const QByteArray bytes = text.toUtf8();
    if (bytes.size() <= managerLimit) return text;
    if (partial) *partial = true;
    const QString header = reportHeader();
    const QString marker = QStringLiteral("[Manager log truncated to fit the report size limit.]\n");
    const int keep = managerLimit - header.toUtf8().size() - marker.toUtf8().size();
    int start = bytes.size() - keep;
    while (start < bytes.size() && (static_cast<unsigned char>(bytes.at(start)) & 0xc0) == 0x80)
        ++start;
    return header + marker + QString::fromUtf8(bytes.constData() + start, bytes.size() - start);
}

QString composeDetailedReport(const QString &managerText, const QVector<StandaloneSnapshot> &snapshots,
                              const QStringList &omitted, const QMap<QString, quint64> &exported,
                              QMap<QString, quint64> *exportedThrough, QSet<QString> *completed,
                              bool *partial)
{
    bool managerPartial = false;
    QString manager = boundedReportManagerText(managerText, MaxReportBytes, &managerPartial);
    bool sizeExceeded = false;
    QString standalone = standaloneSnapshotText(snapshots, omitted, exported, exportedThrough, completed,
        partial, MaxReportBytes - manager.toUtf8().size(), &sizeExceeded);
    if (sizeExceeded) {
        const QString marker = QStringLiteral(
            "\n[Report truncated at 10000000 bytes; unexported events were retained.]\n");
        if (exportedThrough) exportedThrough->clear();
        if (completed) completed->clear();
        manager = boundedReportManagerText(managerText, MaxReportBytes / 2, &managerPartial);
        standalone = standaloneSnapshotText(snapshots, omitted, exported, exportedThrough, completed,
            partial, MaxReportBytes - marker.toUtf8().size() - manager.toUtf8().size(), &sizeExceeded);
        standalone += marker;
    }
    if (managerPartial && partial) *partial = true;
    return manager + standalone;
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
        appendStandaloneRepeatSummary();
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
    const QString fileName = QStringLiteral("standalone-%1.log").arg(instance);
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
    const QByteArray nativeName = QFile::encodeName(fileName);
    QByteArray payload;
    if (pendingDropped) payload += standaloneDroppedLine(pendingDropped);
    for (const StandaloneEvent &event : pending) payload += standaloneEventLine(event);
    struct stat status;
    const bool exists = ::fstatat(directoryFd, nativeName.constData(), &status, AT_SYMLINK_NOFOLLOW) == 0;
    bool ok = exists || errno == ENOENT;
    if (!ok) error = QString::fromLocal8Bit(std::strerror(errno));
    if (ok && exists && (!S_ISREG(status.st_mode) || status.st_nlink != 1)) {
        ok = false;
        error = QStringLiteral("Standalone snapshot is not an ordinary file.");
    }
    if (ok && !exists) {
        QVector<QString> names;
        if (!standaloneSnapshotNames(directoryFd, &names, &error)) {
            ok = false;
        } else if (names.size() >= MaxStandaloneSnapshots) {
            ok = false;
            error = QStringLiteral("Standalone snapshot limit reached.");
        }
        payload.prepend(standaloneHeaderLine(instance, applicationId));
    } else if (ok && qint64(status.st_size) + payload.size() > MaxReportBytes) {
        // Rare compaction: keep the newest half so the file stays bounded.
        QByteArray existingBytes;
        bool present = false;
        StandaloneSnapshot snapshot;
        ok = readStandaloneFile(directoryFd, fileName, &existingBytes, &present, &error);
        if (ok && present) ok = parseStandaloneSnapshot(existingBytes, fileName, &snapshot, &error);
        if (ok && present && snapshot.applicationId != applicationId) {
            ok = false;
            error = QStringLiteral("Standalone snapshot application identity does not match.");
        }
        if (ok) {
            snapshot.instance = instance;
            snapshot.applicationId = applicationId;
            qint64 bytes = 0;
            for (const StandaloneEvent &event : snapshot.events) bytes += standaloneEventLine(event).size();
            while (bytes > MaxReportBytes / 2 && !snapshot.events.isEmpty()) {
                bytes -= standaloneEventLine(dropStandaloneEvent(&snapshot.events)).size();
                if (snapshot.dropped != std::numeric_limits<quint64>::max()) ++snapshot.dropped;
            }
            ok = writeStandaloneFile(directoryFd, fileName, serializeStandaloneSnapshot(snapshot), &error);
        }
    }
    if (ok) {
        const int fd = ::openat(directoryFd, nativeName.constData(),
                                O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0644);
        struct stat opened;
        if (fd < 0 || ::fstat(fd, &opened) < 0 || !S_ISREG(opened.st_mode) || opened.st_nlink != 1) {
            ok = false;
            error = fd < 0 ? QString::fromLocal8Bit(std::strerror(errno))
                           : QStringLiteral("Standalone snapshot is not an ordinary file.");
        }
        int offset = 0;
        while (ok && offset < payload.size()) {
            const ssize_t count = ::write(fd, payload.constData() + offset, size_t(payload.size() - offset));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) {
                ok = false;
                error = QString::fromLocal8Bit(std::strerror(errno));
                // Do not leave a partial line behind.
                if (::ftruncate(fd, opened.st_size) < 0) {}
                break;
            }
            offset += int(count);
        }
        if (fd >= 0 && ::close(fd) < 0 && ok) {
            ok = false;
            error = QString::fromLocal8Bit(std::strerror(errno));
        }
    }
    ::flock(lockFd, LOCK_UN);
    ::close(lockFd);
    ::close(directoryFd);
    if (ok) {
        const quint64 lastWritten = pending.isEmpty() ? 0 : pending.last().sequence;
        QMutexLocker lock(&mutex);
        while (!standaloneWriter.pending.isEmpty() &&
               standaloneWriter.pending.first().sequence <= lastWritten) {
            standaloneWriter.pendingBytes -= standalonePendingEventBytes(standaloneWriter.pending.first());
            standaloneWriter.pending.removeFirst();
        }
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
            if (!validStandaloneFileName(QStringLiteral("standalone-%1.log").arg(instance), nullptr) ||
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
    const QString path = standaloneFilePath;
    standaloneFileBusy.store(true, std::memory_order_release);
    standaloneFileWorker.thread = std::thread([path]() {
        QString text, error;
        bool ok = ErrorLog::detailedReport(&text, &error);
        if (ok) {
            QSaveFile file(path);
            const QByteArray contents = text.toUtf8();
            ok = file.open(QIODevice::WriteOnly) &&
                file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) &&
                file.write(contents) == contents.size() && file.commit();
            if (!ok) error = file.errorString();
        }
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
    if (detailedCaptureEnabled()) appendOverlay(source, message);
    if (standaloneSnapshotMode) {
        if (!MemoryLogBus::clientDetailedEnabled()) {
            appendStandaloneRepeatSummary();
            standaloneWriter.lastErrorSource.clear();
            standaloneWriter.lastErrorMessage.clear();
            return;
        }
        static const QRegularExpression sourcePattern(QStringLiteral("[^A-Za-z0-9_. -]"));
        QString safeSource = source.left(128);
        safeSource.replace(sourcePattern, QStringLiteral("_"));
        if (safeSource.isEmpty()) safeSource = QStringLiteral("Unknown");
        QString boundedMessage = message;
        if (boundedMessage.size() > MaxStandaloneMessageChars)
            boundedMessage = boundedMessage.left(MaxStandaloneMessageChars - 24) +
                             QStringLiteral(" [entry truncated]");
        const QString time = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        const bool canCoalesce = error && !source.isEmpty() && source.size() <= 128 &&
                                 message.size() <= MaxStandaloneMessageChars;
        if (canCoalesce && source == standaloneWriter.lastErrorSource &&
            message == standaloneWriter.lastErrorMessage &&
            standaloneWriter.repeats != std::numeric_limits<quint64>::max()) {
            ++standaloneWriter.repeats;
            standaloneWriter.lastRepeatTime = time;
            return;
        }
        appendStandaloneRepeatSummary();
        standaloneWriter.lastErrorSource = canCoalesce ? source : QString();
        standaloneWriter.lastErrorSafeSource = canCoalesce ? safeSource : QString();
        standaloneWriter.lastErrorMessage = canCoalesce ? message : QString();
        appendStandalonePending(time, safeSource, boundedMessage, error);
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

QString ErrorLog::overlayText()
{
    QMutexLocker lock(&mutex);
    if (!detailedCaptureEnabled()) {
        overlayLines.clear();
        overlayFragments.clear();
        return QString();
    }
    return overlayLines.join(QLatin1Char('\n'));
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
    const QString text = reportHeader() + sharedHistory() + QStringLiteral("Current process context:\n") + recent;
    return writeText(boundedReportManagerText(text, MaxReportBytes, nullptr), path, error);
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
    return (memoryDetailedEnabled.load(std::memory_order_relaxed) &&
            !memoryDetailedPaused.load(std::memory_order_relaxed)) || MemoryLogBus::clientDetailedEnabled();
}

bool ErrorLog::detailedPaused()
{
    return memoryDetailedEnabled.load(std::memory_order_relaxed) &&
        memoryDetailedPaused.load(std::memory_order_relaxed);
}

bool ErrorLog::setDetailedPaused(bool paused)
{
    QMutexLocker lock(&mutex);
    if (!memoryDetailedEnabled) return false;
    memoryDetailedPaused.store(paused, std::memory_order_relaxed);
    if (paused) {
        overlayLines.clear();
        overlayFragments.clear();
    }
    return true;
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
        overlayLines.clear();
        overlayFragments.clear();
        remoteHistory.clear();
        rejectedRemoteEvents = 0;
        memoryDetailedEnabled = enabled;
        memoryDetailedPaused = false;
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

bool ErrorLog::clearCurrentLog(QString *error)
{
    if (!error) return false;
    error->clear();
    {
        QMutexLocker lock(&mutex);
        memoryHistory.clear();
        recent.clear();
        memoryDetailed.clear();
        overlayLines.clear();
        overlayFragments.clear();
        remoteHistory.clear();
        rejectedRemoteEvents = 0;
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

namespace {
// Cached newest part of each append-only standalone log; only new bytes are read on refresh.
struct StandaloneTail {
    quint64 device = 0;
    quint64 inode = 0;
    qint64 offset = 0;
    quint64 lastSequence = 0;
    bool headerRead = false;
    QString label;
    QVector<QString> keys;
    QVector<QString> lines;
    qint64 chars = 0;
};
const qint64 MaxStandaloneTailChars = MaxReportBytes / 4;
QMutex standaloneTailMutex;
QMap<QString, StandaloneTail> standaloneTails;

bool readRange(int fd, qint64 offset, qint64 size, QByteArray *bytes)
{
    bytes->resize(int(size));
    qint64 done = 0;
    while (done < size) {
        const ssize_t count = ::pread(fd, bytes->data() + done, size_t(size - done), off_t(offset + done));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        done += count;
    }
    bytes->resize(int(done));
    return done > 0;
}

void refreshStandaloneTails()
{
    QString path;
    {
        QMutexLocker lock(&mutex);
        path = sharedStandaloneDirectory;
    }
    if (path.isEmpty()) {
        standaloneTails.clear();
        return;
    }
    const int directoryFd = ::open(QFile::encodeName(path).constData(),
                                   O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directoryFd < 0) return;
    QVector<QString> names;
    QString issue;
    if (!standaloneSnapshotNames(directoryFd, &names, &issue)) {
        ::close(directoryFd);
        return;
    }
    if (names.size() > MaxStandaloneSnapshots) names.resize(MaxStandaloneSnapshots);
    for (auto it = standaloneTails.begin(); it != standaloneTails.end();)
        it = names.contains(it.key()) ? std::next(it) : standaloneTails.erase(it);
    for (const QString &name : names) {
        QString instance;
        validStandaloneFileName(name, &instance);
        const int fd = ::openat(directoryFd, QFile::encodeName(name).constData(),
                                O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) continue;
        struct stat status;
        if (::fstat(fd, &status) < 0 || !S_ISREG(status.st_mode) || status.st_nlink != 1) {
            ::close(fd);
            continue;
        }
        StandaloneTail &tail = standaloneTails[name];
        if (tail.device != quint64(status.st_dev) || tail.inode != quint64(status.st_ino) ||
            qint64(status.st_size) < tail.offset) {
            tail = StandaloneTail();
            tail.device = quint64(status.st_dev);
            tail.inode = quint64(status.st_ino);
        }
        if (!tail.headerRead) {
            QByteArray head;
            const int headerEnd = readRange(fd, 0, qMin<qint64>(status.st_size, 512), &head)
                ? head.indexOf('\n') : -1;
            QString applicationId;
            if (headerEnd < 0 || !parseStandaloneHeader(head.left(headerEnd), instance, &applicationId)) {
                ::close(fd);
                continue;
            }
            tail.headerRead = true;
            tail.offset = headerEnd + 1;
            tail.label = applicationId.mid(applicationId.lastIndexOf(QLatin1Char('.')) + 1);
        }
        qint64 start = tail.offset;
        const bool skipPartial = qint64(status.st_size) - start > MaxStandaloneTailChars;
        if (skipPartial) start = qint64(status.st_size) - MaxStandaloneTailChars;
        QByteArray bytes;
        if (qint64(status.st_size) > start && readRange(fd, start, qint64(status.st_size) - start, &bytes)) {
            int first = 0;
            if (skipPartial) first = bytes.indexOf('\n') + 1;
            QVector<StandaloneEvent> events;
            quint64 dropped = 0;
            const int consumed = first > 0 || !skipPartial
                ? parseStandaloneLines(bytes, first, &tail.lastSequence, &events, &dropped) : 0;
            tail.offset = start + consumed;
            for (const StandaloneEvent &event : events) {
                const QString line = QStringLiteral("[%1 UTC] [%2] [%3]%4 %5")
                    .arg(event.time, tail.label, safeSnapshotText(event.source),
                         event.error ? QStringLiteral(" error") : QString(),
                         safeSnapshotText(event.message));
                tail.keys.append(event.time);
                tail.lines.append(line);
                tail.chars += line.size();
            }
            int remove = 0;
            while (tail.chars > MaxStandaloneTailChars && remove < tail.lines.size())
                tail.chars -= tail.lines.at(remove++).size();
            if (remove) {
                tail.keys.remove(0, remove);
                tail.lines.remove(0, remove);
            }
        }
        ::close(fd);
    }
    ::close(directoryFd);
}

// Returns the newest remaining entry of a manager text, newest line first.
bool previousManagerEntry(const QString &text, int *end, QString *key, QStringList *lines)
{
    lines->clear();
    key->clear();
    while (*end > 0) {
        if (text.at(*end - 1) == QLatin1Char('\n')) --*end;
        if (*end <= 0) break;
        const int start = text.lastIndexOf(QLatin1Char('\n'), *end - 1) + 1;
        const QString line = text.mid(start, *end - start);
        *end = start;
        if (!line.trimmed().isEmpty()) lines->append(line);
        const int close = line.startsWith(QLatin1Char('[')) ? line.indexOf(QLatin1String(" UTC]")) : -1;
        if (close > 1 && close <= 41) {
            *key = line.mid(1, close - 1);
            return true;
        }
    }
    return !lines->isEmpty();
}
}

bool ErrorLog::recentLines(const QString &query, int maxLines, QString *text, bool *more,
                           QString *error)
{
    if (!text || !more || !error) return false;
    text->clear();
    error->clear();
    *more = false;
    QMutexLocker tailLock(&standaloneTailMutex);
    {
        QMutexLocker lock(&mutex);
        if (!memoryDetailedEnabled && !MemoryLogBus::clientDetailedEnabled()) {
            *error = QStringLiteral("Detailed logging is not enabled.");
            return false;
        }
    }
    refreshStandaloneTails();
    QMutexLocker lock(&mutex);
    if (!storageError.isEmpty())
        *text += QStringLiteral("[Automatic log storage failed; recent events may be lost.]\n");
    struct Cursor {
        const QString *text = nullptr;
        int end = 0;
        const StandaloneTail *tail = nullptr;
        int index = 0;
        bool valid = false;
        QString key;
        QStringList lines;
    };
    QVector<Cursor> cursors;
    const auto addText = [&](const QString &source) {
        Cursor cursor;
        cursor.text = &source;
        cursor.end = source.size();
        cursors.append(cursor);
    };
    addText(memoryDetailed);
    for (auto it = remoteHistory.constBegin(); it != remoteHistory.constEnd(); ++it)
        addText(it.value().detailed);
    for (auto it = standaloneTails.constBegin(); it != standaloneTails.constEnd(); ++it) {
        Cursor cursor;
        cursor.tail = &it.value();
        cursor.index = it.value().lines.size() - 1;
        cursors.append(cursor);
    }
    const auto advance = [](Cursor &cursor) {
        if (cursor.text) {
            cursor.valid = previousManagerEntry(*cursor.text, &cursor.end, &cursor.key, &cursor.lines);
        } else {
            cursor.valid = cursor.index >= 0;
            if (!cursor.valid) return;
            cursor.key = cursor.tail->keys.at(cursor.index);
            cursor.lines = QStringList() << cursor.tail->lines.at(cursor.index);
            --cursor.index;
        }
    };
    for (Cursor &cursor : cursors) advance(cursor);
    int count = 0;
    while (true) {
        Cursor *newest = nullptr;
        for (Cursor &cursor : cursors)
            if (cursor.valid && (!newest || cursor.key > newest->key)) newest = &cursor;
        if (!newest) break;
        for (const QString &line : newest->lines) {
            if (!query.isEmpty() && !line.contains(query, Qt::CaseInsensitive)) continue;
            if (count >= maxLines) {
                *more = true;
                return true;
            }
            *text += line;
            *text += QLatin1Char('\n');
            ++count;
        }
        advance(*newest);
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
        if (!memoryDetailedEnabled && !MemoryLogBus::clientDetailedEnabled()) {
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
    *text = composeDetailedReport(managerText, snapshots, omitted, exported, nullptr, nullptr, &partial);
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
        if (!memoryDetailedEnabled && !MemoryLogBus::clientDetailedEnabled()) {
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
    QMap<QString, quint64> exportedThrough;
    QSet<QString> completed;
    const QString text = composeDetailedReport(managerText, snapshots, omitted, exported, &exportedThrough,
                                               &completed, &exportPartial);
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
        for (auto it = exportedThrough.constBegin(); it != exportedThrough.constEnd(); ++it)
            exportedStandaloneSequences.insert(it.key(), it.value());
        if (!exportedThrough.isEmpty()) changed();
    }
    if (!exportedThrough.isEmpty() && !storagePath.isEmpty()) {
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
        if (completed.contains(snapshot.instance)) {
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

bool ErrorLog::clearSavedLogs(int &removed, bool includeExports)
{
    removed = 0;
    bool ok = true;
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (includeExports && documents.isEmpty()) {
        ok = false;
    } else if (includeExports) {
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
        if (!sharedStandaloneError.isEmpty()) ok = false;
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
            if (!memoryDetailedEnabled || memoryDetailedPaused) return;
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
            return memoryDetailedEnabled.load(std::memory_order_relaxed) &&
                !memoryDetailedPaused.load(std::memory_order_relaxed);
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
    timer->setInterval(2000);
    QObject::connect(timer, &QTimer::timeout, timer, []() {
        if (standaloneSnapshotBusy.load(std::memory_order_acquire)) return;
        if (standaloneSnapshotWorker.thread.joinable()) standaloneSnapshotWorker.thread.join();
        standaloneSnapshotBusy.store(true, std::memory_order_release);
        standaloneSnapshotWorker.thread = std::thread([]() {
            flushStandaloneWriter();
            standaloneSnapshotBusy.store(false, std::memory_order_release);
        });
    });
    QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, []() {
        finishCoreChannel();
        ErrorLog::flushStandaloneSnapshots();
    });
    timer->start();
    return true;
}

bool ErrorLog::flushStandaloneSnapshots()
{
    if (standaloneSnapshotWorker.thread.joinable()) standaloneSnapshotWorker.thread.join();
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
        finishCoreChannel();
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
            finishCoreChannel();
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

namespace {
struct CorePartial {
    QByteArray text;
    quint32 total;
    int level;
    bool accepted;
    std::chrono::steady_clock::time_point updated;
};

void wakeCoreConsumer()
{
    const char wake = 1;
    if (::send(coreChannel.wakeSender, &wake, 1, MSG_DONTWAIT | MSG_NOSIGNAL) < 0 &&
        errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        std::fprintf(stderr, "Error log channel wake failed: %s\n", std::strerror(errno));
}

void drainCoreMessages()
{
    char wake[256];
    while (::recv(coreChannel.wakeReceiver, wake, sizeof(wake), MSG_DONTWAIT) > 0) {}
    std::deque<CoreMessage> batch;
    bool remaining;
    {
        std::lock_guard<std::mutex> lock(coreChannel.mutex);
        for (int i = 0; i < 256 && !coreChannel.pending.empty(); ++i) {
            coreChannel.bytes -= size_t(coreChannel.pending.front().text.size()) + 32;
            batch.push_back(std::move(coreChannel.pending.front()));
            coreChannel.pending.pop_front();
        }
        remaining = !coreChannel.pending.empty();
    }
    if (remaining) wakeCoreConsumer();
    for (const CoreMessage &message : batch)
        ErrorLog::record(QStringLiteral("Android"), QString::fromUtf8(message.text),
                         message.level >= MEGLER_LOG_ERROR);
}

void receiveCoreMessages()
{
    std::unordered_map<quint32, CorePartial> partials;
    for (;;) {
        pollfd fd = {coreReceiver, POLLIN, 0};
        const int ready = ::poll(&fd, 1, coreChannel.stop.load(std::memory_order_relaxed) ? 0 : 250);
        if (ready == 0 && coreChannel.stop.load(std::memory_order_relaxed)) break;
        if (ready > 0 && !(fd.revents & POLLIN) && (fd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            std::fputs("Error log channel receiver closed unexpectedly.\n", stderr);
            break;
        }
        if (ready > 0 && (fd.revents & POLLIN)) {
            char packet[4096];
            const ssize_t count = ::recv(coreReceiver, packet, sizeof(packet), MSG_DONTWAIT | MSG_TRUNC);
            if (count == 0 && coreChannel.stop.load(std::memory_order_relaxed)) break;
            if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                std::fprintf(stderr, "Error log channel read failed: %s\n", std::strerror(errno));
                break;
            }
            if (count > ssize_t(sizeof(packet))) coreDropped.fetch_add(1, std::memory_order_relaxed);
            if (count >= ssize_t(sizeof(CorePacket)) && count <= ssize_t(sizeof(packet))) {
                CorePacket header;
                std::memcpy(&header, packet, sizeof(header));
                const int payload = int(count - sizeof(header));
                auto it = partials.find(header.id);
                if (header.abort) {
                    if (it != partials.end()) {
                        if (it->second.accepted) {
                            std::lock_guard<std::mutex> lock(coreChannel.mutex);
                            coreChannel.bytes -= size_t(it->second.total) + 32;
                            coreIncomplete.fetch_add(1, std::memory_order_relaxed);
                        }
                        partials.erase(it);
                    }
                    continue;
                }
                if (it == partials.end() && header.offset == 0 &&
                    header.total <= MaxCoreQueueBytes && size_t(payload) <= header.total) {
                    bool accepted;
                    {
                        std::lock_guard<std::mutex> lock(coreChannel.mutex);
                        const size_t cost = size_t(header.total) + 32;
                        accepted = cost <= MaxCoreQueueBytes - coreChannel.bytes;
                        if (accepted) coreChannel.bytes += cost;
                    }
                    CorePartial partial{QByteArray(), header.total, header.level, accepted,
                                        std::chrono::steady_clock::now()};
                    if (accepted) partial.text.reserve(int(header.total));
                    it = partials.emplace(header.id, std::move(partial)).first;
                }
                if (it == partials.end()) continue;
                CorePartial &partial = it->second;
                if (header.total != partial.total || header.level != partial.level ||
                    header.offset > header.total || size_t(payload) > header.total - header.offset ||
                    (partial.accepted && header.offset != quint32(partial.text.size()))) {
                    if (partial.accepted) {
                        std::lock_guard<std::mutex> lock(coreChannel.mutex);
                        coreChannel.bytes -= size_t(partial.total) + 32;
                        coreDropped.fetch_add(1, std::memory_order_relaxed);
                    }
                    partials.erase(it);
                    continue;
                }
                partial.updated = std::chrono::steady_clock::now();
                if (partial.accepted) partial.text.append(packet + sizeof(header), payload);
                if (header.offset + quint32(payload) == header.total) {
                    if (partial.accepted) {
                        {
                            std::lock_guard<std::mutex> lock(coreChannel.mutex);
                            coreChannel.pending.push_back(CoreMessage{partial.level, std::move(partial.text)});
                        }
                        wakeCoreConsumer();
                    } else coreDropped.fetch_add(1, std::memory_order_relaxed);
                    partials.erase(it);
                }
            }
        } else if (ready < 0 && errno != EINTR) {
            std::fprintf(stderr, "Error log channel poll failed: %s\n", std::strerror(errno));
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        for (auto it = partials.begin(); it != partials.end();) {
            if (now - it->second.updated < std::chrono::seconds(30)) {
                ++it;
                continue;
            }
            if (it->second.accepted) {
                std::lock_guard<std::mutex> lock(coreChannel.mutex);
                coreChannel.bytes -= size_t(it->second.total) + 32;
            }
            it = partials.erase(it);
        }
    }
    for (const auto &partial : partials) {
        if (partial.second.accepted) {
            std::lock_guard<std::mutex> lock(coreChannel.mutex);
            coreChannel.bytes -= size_t(partial.second.total) + 32;
            coreIncomplete.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void reportCoreLosses()
{
    const unsigned int dropped = coreDropped.exchange(0, std::memory_order_relaxed);
    if (dropped)
        ErrorLog::record(QStringLiteral("Android"),
                         QStringLiteral("Android log channel: %1 events dropped").arg(dropped), false);
    const unsigned int incomplete = coreIncomplete.exchange(0, std::memory_order_relaxed);
    if (incomplete)
        ErrorLog::record(QStringLiteral("Android"),
                         QStringLiteral("Android log channel: %1 incomplete events discarded").arg(incomplete),
                         false);
}

void finishCoreChannel()
{
    if (!coreChannel.worker.joinable()) return;
    if (::shutdown(coreSender, SHUT_WR) < 0)
        std::fprintf(stderr, "Error log channel shutdown failed: %s\n", std::strerror(errno));
    coreChannel.stop.store(true, std::memory_order_relaxed);
    coreChannel.worker.join();
    for (;;) {
        drainCoreMessages();
        std::lock_guard<std::mutex> lock(coreChannel.mutex);
        if (coreChannel.pending.empty()) break;
    }
    reportCoreLosses();
}
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
    int wake[2];
    if (::socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, wake) < 0) {
        if (error) *error = QString::fromLocal8Bit(std::strerror(errno));
        ::close(sockets[0]);
        ::close(sockets[1]);
        return false;
    }
    coreSender = sockets[0];
    coreReceiver = sockets[1];
    coreChannel.wakeSender = wake[0];
    coreChannel.wakeReceiver = wake[1];
    try {
        coreChannel.worker = std::thread(receiveCoreMessages);
    } catch (const std::system_error &failure) {
        if (error) *error = QString::fromLocal8Bit(failure.what());
        ::close(coreSender);
        ::close(coreReceiver);
        ::close(coreChannel.wakeSender);
        ::close(coreChannel.wakeReceiver);
        coreSender = coreReceiver = coreChannel.wakeSender = coreChannel.wakeReceiver = -1;
        return false;
    }
    QSocketNotifier *notifier = new QSocketNotifier(coreChannel.wakeReceiver, QSocketNotifier::Read);
    QObject::connect(notifier, &QSocketNotifier::activated, notifier, [](int) { drainCoreMessages(); });
    QTimer *lossTimer = new QTimer(notifier);
    lossTimer->setInterval(250);
    QObject::connect(lossTimer, &QTimer::timeout, notifier, []() {
        drainCoreMessages();
        reportCoreLosses();
    });
    QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, notifier,
                     []() { finishCoreChannel(); });
    lossTimer->start();
    return true;
}

void ErrorLog::sendCoreLine(int level, const char *line)
{
    if (coreSender < 0 || !line) return;
    const int savedErrno = errno;
    size_t length = 0;
    while (length <= MaxCoreQueueBytes && line[length]) ++length;
    if (length > MaxCoreQueueBytes) {
        coreDropped.fetch_add(1, std::memory_order_relaxed);
        errno = savedErrno;
        return;
    }
    const quint32 id = coreChannel.nextId.fetch_add(1, std::memory_order_relaxed);
    size_t offset = 0;
    do {
        const size_t chunk = qMin(length - offset, size_t(4096 - sizeof(CorePacket)));
        CorePacket header{id, quint32(offset), quint32(length), level, 0};
        iovec parts[2] = {{&header, sizeof(header)}, {const_cast<char *>(line + offset), chunk}};
        msghdr message = {};
        message.msg_iov = parts;
        message.msg_iovlen = 2;
        ssize_t sent;
        do {
            sent = ::sendmsg(coreSender, &message, MSG_DONTWAIT | MSG_NOSIGNAL);
        } while (sent < 0 && errno == EINTR);
        if (sent < 0) {
            coreDropped.fetch_add(1, std::memory_order_relaxed);
            header.abort = 1;
            ::send(coreSender, &header, sizeof(header), MSG_DONTWAIT | MSG_NOSIGNAL);
            break;
        }
        offset += chunk;
    } while (offset < length);
    errno = savedErrno;
}
