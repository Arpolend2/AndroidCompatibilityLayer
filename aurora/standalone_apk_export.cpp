#include "standalone_apk_export.h"
#include "manager_identity.h"

#include "install_request_candidate.h"
#include "install_request_snapshot.h"

#include <QDateTime>
#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QWaitCondition>

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <QCoreApplication>
#include <functional>
#include <memory>

struct StandaloneLaunchResult {
    QMutex mutex;
    QWaitCondition ready;
    bool done = false;
    bool accepted = false;
};

namespace {
const qint64 HandoffTtl = 30 * 60;
const int MaxHandoffs = 8;
const char ManagerService[] = "ru.androidcompatibilitylayer.appmanager";
const char ManagerPath[] = "/ru/androidcompatibilitylayer/appmanager";

class VerifiedManagerCall : public QObject
{
public:
    using Completion = std::function<void(const QString &, const QString &)>;
    VerifiedManagerCall(const QString &method, const QStringList &arguments, bool autoStart,
                        const Completion &completion, QObject *parent, int timeout = 5000)
        : QObject(parent), m_completion(completion)
    {
        QDBusMessage call = QDBusMessage::createMethodCall(QString::fromLatin1(ManagerService),
            QString::fromLatin1(ManagerPath), QString::fromLatin1(ManagerService), method);
        call.setAutoStartService(autoStart);
        for (const QString &argument : arguments) call << argument;
        auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call, timeout), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher]() {
            const QDBusMessage reply = watcher->reply();
            watcher->deleteLater();
            if (reply.type() == QDBusMessage::ErrorMessage) {
                done(QString(), QStringLiteral("Manager D-Bus error %1: %2")
                     .arg(reply.errorName(), reply.errorMessage()));
                return;
            }
            if (reply.type() != QDBusMessage::ReplyMessage || !reply.service().startsWith(QLatin1Char(':')) ||
                reply.arguments().size() != 1 || reply.arguments().first().type() != QVariant::String) {
                done(QString(), QStringLiteral("Manager reply unavailable or invalid."));
                return;
            }
            const QString value = reply.arguments().first().toString();
            QDBusMessage identity = QDBusMessage::createMethodCall(QStringLiteral("ru.omp.am"),
                QStringLiteral("/ru/omp/am"), QStringLiteral("ru.omp.am.Identify"),
                QStringLiteral("IdentifyByBusName"));
            identity << reply.service();
            auto *check = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(identity, 5000), this);
            connect(check, &QDBusPendingCallWatcher::finished, this, [this, check, value]() {
                const QDBusMessage identity = check->reply();
                check->deleteLater();
                if (identity.type() == QDBusMessage::ErrorMessage) {
                    done(QString(), QStringLiteral("Manager identity D-Bus error %1: %2")
                         .arg(identity.errorName(), identity.errorMessage()));
                    return;
                }
                const QList<QVariant> fields = identity.arguments();
                if (identity.type() != QDBusMessage::ReplyMessage || fields.size() != 2 ||
                    fields.at(0).type() != QVariant::String || fields.at(1).type() != QVariant::String) {
                    done(QString(), QStringLiteral("Manager identity could not be verified: invalid reply format."));
                    return;
                }
                const QString executable = fields.at(0).toString();
                const QString key = fields.at(1).toString();
                const bool expected = isTrustedAppManagerExecutable(executable);
                if (!expected || key.isEmpty() || key.size() > MaxAppManagerIdentityKeyLength) {
                    const QString pathCategory = expected ? QStringLiteral("expected")
                        : executable.isEmpty() ? QStringLiteral("empty")
                        : QFileInfo(executable).fileName() == QLatin1String("dbus-proxy")
                            ? QStringLiteral("dbus-proxy") : QStringLiteral("other");
                    const QString keyCategory = key.isEmpty() ? QStringLiteral("empty")
                        : key.size() > MaxAppManagerIdentityKeyLength ? QStringLiteral("oversized")
                        : QStringLiteral("present");
                    done(QString(), QStringLiteral("Manager identity could not be verified: executable=%1, key=%2.")
                         .arg(pathCategory, keyCategory));
                    return;
                }
                done(value, QString());
            });
        });
    }
private:
    void done(const QString &value, const QString &error)
    {
        m_completion(value, error);
        deleteLater();
    }
    Completion m_completion;
};

class ResultWaiter : public QObject
{
public:
    ResultWaiter(const QString &id, const QByteArray &fingerprint,
                 const std::function<void(bool, const QString &)> &completion, QObject *parent)
        : QObject(parent), m_id(id), m_fingerprint(fingerprint), m_completion(completion)
    {
        m_clock.start();
        m_timer.setInterval(2000);
        connect(&m_timer, &QTimer::timeout, this, [this]() { poll(); });
        m_timer.start();
        QTimer::singleShot(0, this, [this]() { poll(); });
    }
private:
    void finish(bool success, const QString &message)
    {
        if (m_done) return;
        m_done = true;
        m_timer.stop();
        m_completion(success, message);
        deleteLater();
    }
    void poll()
    {
        if (m_clock.elapsed() >= 30 * 60 * 1000) {
            finish(false, tr("The manager did not confirm the APK import in time."));
            return;
        }
        if (m_pending || m_done) return;
        m_pending = true;
        new VerifiedManagerCall(QStringLiteral("storeResult"), QStringList() << m_id, false,
            [this](const QString &result, const QString &error) {
                if (m_done) return;
                m_pending = false;
                if (m_clock.elapsed() >= 30 * 60 * 1000) {
                    finish(false, tr("The manager did not confirm the APK import in time."));
                    return;
                }
                if (!error.isEmpty()) {
                    if (!m_warned) {
                        qWarning("APK handoff: %s", qPrintable(error));
                        m_warned = true;
                    }
                    return;
                }
                m_warned = false;
                if (result == QLatin1String("failed")) {
                    qInfo("APK handoff: manager reported import failure.");
                    finish(false, tr("The manager could not add the APK to the library."));
                } else if (result.startsWith(QLatin1String("installed:"))) {
                    if (result.mid(10).toLatin1() == m_fingerprint) {
                        qInfo("APK handoff: manager confirmed the imported snapshot.");
                        finish(true, tr("The APK was added to the manager library."));
                    } else
                        finish(false, tr("The imported APK does not match the request."));
                } else if (!result.isEmpty()) {
                    finish(false, tr("The manager returned an invalid APK import result."));
                }
            }, this);
    }
    QString m_id;
    QByteArray m_fingerprint;
    std::function<void(bool, const QString &)> m_completion;
    QElapsedTimer m_clock;
    QTimer m_timer;
    bool m_pending = false;
    bool m_warned = false;
    bool m_done = false;
};

bool validLibraryList(const QString &contents)
{
    if (contents.size() > 1024 * 1024 ||
        !contents.startsWith(QLatin1String("MEGLER_INSTALL_STATUS_V1\n")) ||
        !contents.endsWith(QLatin1Char('\n'))) return false;
    static const QRegularExpression package(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)+$"));
    QSet<QString> seen;
    const QStringList lines = contents.split(QLatin1Char('\n'));
    for (int i = 1; i + 1 < lines.size(); ++i) {
        const QStringList fields = lines.at(i).split(QLatin1Char('\t'));
        bool version = false;
        if (fields.size() != 5 || fields.at(0).size() > 159 || !package.match(fields.at(0)).hasMatch() ||
            seen.contains(fields.at(0)) || lines.at(i).contains(QLatin1Char('\r'))) return false;
        fields.at(4).toUInt(&version);
        if (!version) return false;
        seen.insert(fields.at(0));
    }
    return seen.size() <= 4096;
}

struct Fd {
    explicit Fd(int fd = -1) : value(fd) {}
    ~Fd() { if (value >= 0) close(value); }
    int value;
};

int handoffDirectory(const QString &sharedRoot, QString *error)
{
    if (sharedRoot.isEmpty() || !QDir::isAbsolutePath(sharedRoot) ||
        QDir::cleanPath(sharedRoot) != sharedRoot || !QDir().mkpath(sharedRoot)) {
        *error = QCoreApplication::translate("ExportThread", "Cannot create the shared installation directory.");
        return -1;
    }
    const QByteArray root = QFile::encodeName(sharedRoot);
    Fd parent(open(root.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (parent.value < 0 ||
        (mkdirat(parent.value, "store-install-handoffs", 0700) != 0 && errno != EEXIST)) {
        *error = QCoreApplication::translate("ExportThread", "Cannot open the shared installation directory.");
        return -1;
    }
    int fd = openat(parent.value, "store-install-handoffs", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) *error = QCoreApplication::translate("ExportThread", "Cannot open the shared installation directory safely.");
    return fd;
}

bool cleanAndCount(int dir, int *count, QString *error)
{
    const int scanFd = openat(dir, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR *entries = scanFd < 0 ? nullptr : fdopendir(scanFd);
    if (!entries) {
        if (scanFd >= 0) close(scanFd);
        *error = QCoreApplication::translate("ExportThread", "Cannot inspect shared installation requests.");
        return false;
    }
    static const QRegularExpression pattern(QStringLiteral("^(request|pending|claimed)_[0-9a-f]{32}\\.(apk|apks|xapk)$"));
    const qint64 cutoff = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch() / 1000 - HandoffTtl;
    *count = 0;
    int visited = 0;
    errno = 0;
    while (dirent *entry = readdir(entries)) {
        if (++visited > 4096) {
            *error = QCoreApplication::translate("ExportThread", "The shared installation queue cannot be inspected safely.");
            closedir(entries);
            return false;
        }
        const QString name = QString::fromLatin1(entry->d_name);
        if (!pattern.match(name).hasMatch()) continue;
        struct stat st;
        if (fstatat(dir, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) { errno = 0; continue; }
            *error = QCoreApplication::translate("ExportThread", "Cannot inspect a shared installation request.");
            closedir(entries);
            return false;
        }
        if (S_ISREG(st.st_mode) && st.st_nlink == 1 && st.st_mtime <= cutoff) {
            if (unlinkat(dir, entry->d_name, 0) == 0 || errno == ENOENT) { errno = 0; continue; }
            *error = QCoreApplication::translate("ExportThread", "Cannot remove an expired installation request.");
            closedir(entries);
            return false;
        }
        ++*count;
        errno = 0;
    }
    const int readError = errno;
    closedir(entries);
    if (readError) {
        *error = QCoreApplication::translate("ExportThread", "Cannot inspect shared installation requests.");
        return false;
    }
    return true;
}

class ExportThread : public QThread
{
public:
    QString root, sharedRoot, source, name, output, error;
    QByteArray fingerprint;
    void run() override
    {
        InstallRequestCandidate candidate;
        InstallRequestSnapshot bundle;
        QString snapshotPath;
        const bool isBundle = name.endsWith(QLatin1String(".apks")) ||
                              name.endsWith(QLatin1String(".xapk"));
        if (isBundle) {
            std::string why;
            if (!bundle.capture(root.toUtf8().toStdString(), source.toUtf8().toStdString(),
                                name.toUtf8().toStdString(), 512u * 1024 * 1024, &why, true)) {
                error = QString::fromStdString(why);
                return;
            }
            snapshotPath = QString::fromStdString(bundle.path());
        } else {
            if (!candidate.prepare(root, source, name, 512u * 1024 * 1024, &error)) return;
            snapshotPath = candidate.path();
        }
        Fd dir(handoffDirectory(sharedRoot, &error));
        if (dir.value < 0) return;
        if (flock(dir.value, LOCK_EX | LOCK_NB) != 0) {
            error = QCoreApplication::translate("ExportThread", "The shared installation queue is busy.");
            return;
        }
        int count = 0;
        if (!cleanAndCount(dir.value, &count, &error)) return;
        if (count >= MaxHandoffs) {
            error = QCoreApplication::translate("ExportThread", "The shared installation queue is full. Try again later.");
            return;
        }
        QString id = QUuid::createUuid().toString();
        id.remove(QLatin1Char('{'));
        id.remove(QLatin1Char('}'));
        id.remove(QLatin1Char('-'));
        const QString extension = name.endsWith(QLatin1String(".xapk")) ? QStringLiteral(".xapk")
            : isBundle ? QStringLiteral(".apks") : QStringLiteral(".apk");
        const QString fileName = QStringLiteral("request_") + id + extension;
        const QByteArray nameBytes = fileName.toLatin1();
        const QByteArray pendingBytes = (QStringLiteral("pending_") + id + extension).toLatin1();
        output = QDir(sharedRoot).filePath(QStringLiteral("store-install-handoffs/") + fileName);
        QFile input(snapshotPath);
        if (!input.open(QIODevice::ReadOnly)) {
            qWarning("APK handoff: cannot open snapshot: %s", qPrintable(input.errorString()));
            error = QCoreApplication::translate("ExportThread", "Could not open the APK snapshot.");
            output.clear();
            return;
        }
        Fd destination(openat(dir.value, pendingBytes.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        QFile file;
        if (destination.value < 0 || !file.open(destination.value, QIODevice::WriteOnly)) {
            qWarning("APK handoff: cannot create export: %s", qPrintable(file.errorString()));
            if (destination.value >= 0) unlinkat(dir.value, pendingBytes.constData(), 0);
            error = QCoreApplication::translate("ExportThread", "Could not create the shared APK handoff.");
            output.clear();
            return;
        }
        char buffer[65536];
        QCryptographicHash hash(QCryptographicHash::Sha256);
        for (;;) {
            const qint64 count = input.read(buffer, sizeof(buffer));
            if (count < 0 || (count > 0 && file.write(buffer, count) != count)) {
                error = QCoreApplication::translate("ExportThread", "Could not copy the APK for handoff.");
                file.close();
                unlinkat(dir.value, pendingBytes.constData(), 0);
                output.clear();
                return;
            }
            if (!count) break;
            hash.addData(buffer, static_cast<int>(count));
        }
        if (!file.flush() || fsync(destination.value) != 0) {
            error = QCoreApplication::translate("ExportThread", "Could not save the APK handoff.");
            file.close();
            unlinkat(dir.value, pendingBytes.constData(), 0);
            output.clear();
            return;
        }
#if defined(SYS_renameat2)
        if (syscall(SYS_renameat2, dir.value, pendingBytes.constData(), dir.value, nameBytes.constData(), 1u) != 0) {
            unlinkat(dir.value, pendingBytes.constData(), 0);
            error = QCoreApplication::translate("ExportThread", "Could not publish the APK handoff safely.");
            output.clear();
        } else if (fsync(dir.value) != 0) {
            unlinkat(dir.value, nameBytes.constData(), 0);
            error = QCoreApplication::translate("ExportThread", "Could not save the APK handoff reliably.");
            output.clear();
        } else fingerprint = hash.result().toHex();
#else
        unlinkat(dir.value, pendingBytes.constData(), 0);
        error = QCoreApplication::translate("ExportThread", "This platform cannot publish an APK handoff safely.");
        output.clear();
#endif
    }
};
}

StandaloneApkExport::StandaloneApkExport(const QString &dataRoot, const QString &sharedRoot,
                                         const megler_system_host &other, QObject *parent,
                                         const QString &libraryCachePath, LaunchMode launchMode)
    : QObject(parent), m_dataRoot(dataRoot), m_sharedRoot(sharedRoot), m_libraryCachePath(libraryCachePath),
      m_other(other), m_launchMode(launchMode)
{
    qRegisterMetaType<StandaloneLaunchResultPtr>("StandaloneLaunchResultPtr");
    connect(this, &StandaloneApkExport::launchRequested, this,
            &StandaloneApkExport::processLaunch, Qt::QueuedConnection);
    if (!m_directory.isValid() || !QDir(m_directory.path()).mkdir(QStringLiteral("installs"))) {
        m_error = tr("Could not create the APK handoff directory.");
        return;
    }
    QSaveFile marker(m_directory.path() + QStringLiteral("/apps.txt"));
    QByteArray contents("MEGLER_INSTALL_STATUS_V1\nMEGLER_LIBRARY_UNAVAILABLE\n");
    QFile cache(m_libraryCachePath);
    if (!m_libraryCachePath.isEmpty() && cache.open(QIODevice::ReadOnly)) {
        const QByteArray saved = cache.read(1024 * 1024 + 1);
        if (validLibraryList(QString::fromUtf8(saved))) contents = saved;
        else qWarning("APK handoff: cached library list is invalid.");
    }
    if (!marker.open(QIODevice::WriteOnly) || marker.write(contents) != contents.size() || !marker.commit()) {
        m_error = tr("Could not initialize the APK handoff.");
        return;
    }
    connect(this, &StandaloneApkExport::requested, this, &StandaloneApkExport::processRequest, Qt::QueuedConnection);
    QTimer *cleanup = new QTimer(this);
    cleanup->setInterval(60000);
    connect(cleanup, &QTimer::timeout, this, [this]() {
        QString error;
        Fd dir(handoffDirectory(m_sharedRoot, &error));
        int count = 0;
        if (dir.value < 0 || flock(dir.value, LOCK_EX | LOCK_NB) != 0 ||
            !cleanAndCount(dir.value, &count, &error))
            qWarning("APK handoff: expired requests could not be cleaned: %s", qPrintable(error));
    });
    cleanup->start();
    QTimer *library = new QTimer(this);
    library->setInterval(5000);
    connect(library, &QTimer::timeout, this, &StandaloneApkExport::refreshLibrary);
    library->start();
    QTimer::singleShot(0, this, [this]() { refreshLibrary(); });
}

QString StandaloneApkExport::appsDir() const { return m_error.isEmpty() ? m_directory.path() : QString(); }

void StandaloneApkExport::refreshLibrary()
{
    if (m_libraryPending) return;
    m_libraryPending = true;
    new VerifiedManagerCall(QStringLiteral("libraryApps"), QStringList(), true,
        [this](const QString &contents, const QString &error) {
            m_libraryPending = false;
            if (!error.isEmpty() || !validLibraryList(contents)) {
                if (!m_libraryWarned)
                    qWarning("APK handoff: library list unavailable or unverified: %s",
                             qPrintable(error.isEmpty() ? QStringLiteral("Invalid library data.") : error));
                m_libraryWarned = true;
                return;
            }
            m_libraryWarned = false;
            const QByteArray data = contents.toUtf8();
            QSaveFile marker(m_directory.path() + QStringLiteral("/apps.txt"));
            if (!marker.open(QIODevice::WriteOnly) || marker.write(data) != data.size() || !marker.commit()) {
                qWarning("APK handoff: cannot update library list: %s", qPrintable(marker.errorString()));
                return;
            }
            if (!m_libraryCachePath.isEmpty()) {
                QSaveFile cache(m_libraryCachePath);
                if (!cache.open(QIODevice::WriteOnly) || cache.write(data) != data.size() || !cache.commit())
                    qWarning("APK handoff: cannot cache library list: %s", qPrintable(cache.errorString()));
            }
        }, this);
}

QString StandaloneApkExport::takeNext(const QString &sharedRoot, QString *error)
{
    if (error) error->clear();
    QString why;
    Fd dir(handoffDirectory(sharedRoot, &why));
    if (dir.value < 0 || flock(dir.value, LOCK_EX | LOCK_NB) != 0) {
        if (error) *error = why.isEmpty() ? tr("The shared installation queue is busy.") : why;
        return QString();
    }
    int count = 0;
    if (!cleanAndCount(dir.value, &count, &why)) {
        if (error) *error = why;
        return QString();
    }
    const int scanFd = openat(dir.value, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR *entries = scanFd < 0 ? nullptr : fdopendir(scanFd);
    if (!entries) {
        if (scanFd >= 0) close(scanFd);
        if (error) *error = tr("Cannot inspect shared installation requests.");
        return QString();
    }
    static const QRegularExpression pattern(QStringLiteral("^(request|claimed)_([0-9a-f]{32})(\\.apk|\\.apks|\\.xapk)$"));
    QString result;
    int visited = 0;
    for (;;) {
        errno = 0;
        dirent *entry = readdir(entries);
        if (!entry) {
            if (errno) why = tr("Cannot inspect shared installation requests.");
            break;
        }
        if (++visited > 4096) {
            why = tr("The shared installation queue cannot be inspected safely.");
            break;
        }
        const QRegularExpressionMatch match = pattern.match(QString::fromLatin1(entry->d_name));
        if (!match.hasMatch()) continue;
        struct stat st;
        if (fstatat(dir.value, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_size < 0 ||
            st.st_size > 512LL * 1024 * 1024) {
            why = tr("A shared installation request is invalid.");
            break;
        }
        const QString claimed = QStringLiteral("claimed_") + match.captured(2) + match.captured(3);
        if (match.captured(1) == QLatin1String("claimed")) {
            result = QDir(sharedRoot).filePath(QStringLiteral("store-install-handoffs/") + claimed);
            break;
        }
#if defined(SYS_renameat2)
        const QByteArray target = claimed.toLatin1();
        if (syscall(SYS_renameat2, dir.value, entry->d_name, dir.value, target.constData(), 1u) != 0) {
            why = tr("Cannot claim the shared installation request safely.");
            break;
        }
        result = QDir(sharedRoot).filePath(QStringLiteral("store-install-handoffs/") + claimed);
#else
        why = tr("This platform cannot claim an installation request safely.");
#endif
        break;
    }
    closedir(entries);
    if (error) *error = why;
    return result;
}

bool StandaloneApkExport::removeClaimed(const QString &sharedRoot, const QString &path, QString *error)
{
    if (error) error->clear();
    const QFileInfo file(path);
    static const QRegularExpression pattern(QStringLiteral("^claimed_[0-9a-f]{32}\\.(apk|apks|xapk)$"));
    if (file.absolutePath() != QDir(sharedRoot).filePath(QStringLiteral("store-install-handoffs")) ||
        !pattern.match(file.fileName()).hasMatch()) {
        if (error) *error = tr("Invalid shared installation request.");
        return false;
    }
    QString why;
    Fd dir(handoffDirectory(sharedRoot, &why));
    if (dir.value < 0 || flock(dir.value, LOCK_EX | LOCK_NB) != 0) {
        if (error) *error = why.isEmpty() ? tr("The shared installation queue is busy.") : why;
        return false;
    }
    const QByteArray name = file.fileName().toLatin1();
    struct stat st;
    if (fstatat(dir.value, name.constData(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) return true;
        if (error) *error = tr("Cannot inspect the shared installation request.");
        return false;
    }
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1 ||
        (unlinkat(dir.value, name.constData(), 0) != 0 && errno != ENOENT)) {
        if (error) *error = tr("Cannot remove the shared installation request safely.");
        return false;
    }
    return true;
}

megler_system_host StandaloneApkExport::host()
{
    megler_system_host value = {};
    value.user = this;
    value.open_url = m_other.open_url ? openUrl : nullptr;
    value.share_text = m_other.share_text ? shareText : nullptr;
    value.install_apk = m_error.isEmpty() ? install : nullptr;
    value.launch_app = launch;
    value.uninstall_app = m_other.uninstall_app ? uninstall : nullptr;
    value.permission = m_other.permission ? permission : nullptr;
    value.pick = m_other.pick ? pick : nullptr;
    return value;
}

void MEGLER_CB StandaloneApkExport::openUrl(void *u, const char *uri) { auto s = static_cast<StandaloneApkExport *>(u); s->m_other.open_url(s->m_other.user, uri); }
void MEGLER_CB StandaloneApkExport::shareText(void *u, const char *text, const char *subject) { auto s = static_cast<StandaloneApkExport *>(u); s->m_other.share_text(s->m_other.user, text, subject); }
bool MEGLER_CB StandaloneApkExport::launch(void *u, const char *pkg, const char *)
{
    auto self = static_cast<StandaloneApkExport *>(u);
    if (!pkg || strnlen(pkg, 160) >= 160) return false;
    const QString package = QString::fromUtf8(pkg);
    static const QRegularExpression valid(QStringLiteral("\\A[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)+\\z"));
    if (!valid.match(package).hasMatch()) return false;
    const bool queued = self->m_launchMode == QueueLaunch;
    if (!queued && QThread::currentThread() == self->thread()) {
        qWarning("App launch requested on the UI thread; the asynchronous result cannot be returned.");
        return false;
    }
    if (queued && self->m_launchPending.exchange(true)) {
        qWarning("App launch ignored: another launch is pending.");
        return false;
    }
    const StandaloneLaunchResultPtr state = queued ? StandaloneLaunchResultPtr()
                                                    : std::make_shared<StandaloneLaunchResult>();
    QMetaObject::Connection destroyed;
    if (state) {
        destroyed = QObject::connect(self, &QObject::destroyed, [state]() {
            QMutexLocker lock(&state->mutex);
            if (!state->done) {
                state->done = true;
                state->ready.wakeAll();
            }
        });
    }
    emit self->launchRequested(package, state);
    if (queued) return true;
    QMutexLocker lock(&state->mutex);
    QElapsedTimer clock;
    clock.start();
    while (!state->done) {
        const qint64 remaining = 50000 - clock.elapsed();
        if (remaining <= 0) break;
        state->ready.wait(&state->mutex, static_cast<unsigned long>(remaining));
    }
    const bool accepted = state->done && state->accepted;
    lock.unlock();
    QObject::disconnect(destroyed);
    return accepted;
}

void StandaloneApkExport::processLaunch(const QString &package, const StandaloneLaunchResultPtr &state)
{
    new VerifiedManagerCall(QStringLiteral("launchApp"), QStringList() << package, true,
        [this, state](const QString &result, const QString &error) {
            const bool accepted = error.isEmpty() && result == QLatin1String("ok");
            if (!accepted)
                qWarning("App launch failed: %s", qPrintable(error.isEmpty() ? result : error));
            if (!state) {
                m_launchPending.store(false);
                return;
            }
            QMutexLocker lock(&state->mutex);
            if (!state->done) {
                state->accepted = accepted;
                state->done = true;
                state->ready.wakeAll();
            }
        }, this, 40000);
}
bool MEGLER_CB StandaloneApkExport::uninstall(void *u, const char *pkg) { auto s = static_cast<StandaloneApkExport *>(u); return s->m_other.uninstall_app(s->m_other.user, pkg); }
int MEGLER_CB StandaloneApkExport::permission(void *u, const char *group, bool ask) { auto s = static_cast<StandaloneApkExport *>(u); return s->m_other.permission(s->m_other.user, group, ask); }
bool MEGLER_CB StandaloneApkExport::pick(void *u, const megler_pick_request *request) { auto s = static_cast<StandaloneApkExport *>(u); return s->m_other.pick(s->m_other.user, request); }

bool MEGLER_CB StandaloneApkExport::install(void *user, const char *path, const char *name)
{
    auto self = static_cast<StandaloneApkExport *>(user);
    if (!path || !name) return false;
    const size_t pathLength = strnlen(path, 4097), nameLength = strnlen(name, 256);
    if (!pathLength || pathLength > 4096 || nameLength > 255) return false;
    const QString request = QString::fromLatin1(name, static_cast<int>(nameLength));
    static const QRegularExpression pattern(QStringLiteral("^request_[0-9a-f]{32}(_[0-9]{1,10}_[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)+)?\\.(apk|apks|xapk)$"));
    if (!pattern.match(request).hasMatch()) return false;
    QMutexLocker lock(&self->m_mutex);
    if (self->m_busy || self->m_pendingRequests.size() >= 16 || self->m_pendingRequests.contains(request)) return false;
    self->m_busy = true;
    self->m_pendingRequests.insert(request);
    emit self->requested(QString::fromUtf8(path, static_cast<int>(pathLength)), request);
    return true;
}

void StandaloneApkExport::processRequest(const QString &path, const QString &name)
{
    ExportThread *worker = new ExportThread;
    worker->root = m_dataRoot;
    worker->sharedRoot = m_sharedRoot;
    worker->source = path;
    worker->name = name;
    connect(worker, &QThread::finished, this, [this, worker, name]() {
        const QString error = worker->error;
        const QString output = worker->output;
        const QByteArray fingerprint = worker->fingerprint;
        worker->deleteLater();
        if (!error.isEmpty()) {
            reportFailure(name, error);
            QMutexLocker lock(&m_mutex);
            m_busy = false;
            m_pendingRequests.remove(name);
            return;
        }
        const QString id = QFileInfo(output).fileName().mid(8, 32);
        new ResultWaiter(id, fingerprint, [this, name](bool success, const QString &message) {
            reportResult(name, success, message);
            QMutexLocker lock(&m_mutex);
            m_pendingRequests.remove(name);
        }, this);
        QMutexLocker lock(&m_mutex);
        m_busy = false;
    });
    worker->start();
}

void StandaloneApkExport::reportFailure(const QString &name, const QString &message)
{
    reportResult(name, false, message);
}

void StandaloneApkExport::reportResult(const QString &name, bool success, const QString &message)
{
    QString reason = message;
    reason.replace(QLatin1Char('\n'), QLatin1Char(' '));
    reason.replace(QLatin1Char('\r'), QLatin1Char(' '));
    reason.replace(QLatin1Char('\t'), QLatin1Char(' '));
    QSaveFile result(m_directory.path() + QStringLiteral("/installs/") + name + QStringLiteral(".txt"));
    const QByteArray contents = QByteArray(success ? "1\tinstalled\t" : "1\tfailed\t") + reason.toUtf8();
    if (!result.open(QIODevice::WriteOnly) || result.write(contents) != contents.size() || !result.commit())
        qWarning("Could not report APK handoff failure: %s", qPrintable(result.errorString()));
}
