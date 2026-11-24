#include "manager_lifecycle.h"
#include "standalone_diagnostic_registry.h"
#include "preview_keyboard.h"
#include "preview_orientation.h"
#include "preview_viewport.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMouseEvent>
#include <QPainter>
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScreen>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>

namespace {
const char ServiceName[] = "ru.androidcompatibilitylayer.appmanager";
const char ObjectPath[] = "/ru/androidcompatibilitylayer/appmanager";
const qint64 maxAppMetadataBytes = 1024 * 1024;
const qint64 maxAppsListBytes = 4 * 1024 * 1024;

bool readBoundedFile(const QString &path, qint64 limit, QByteArray *contents, bool *exists,
                     QString *error)
{
    QFile file(path);
    *exists = file.exists();
    if (!*exists) {
        contents->clear();
        return true;
    }
    if (QFileInfo(path).isSymLink() || !file.open(QIODevice::ReadOnly) ||
        file.size() < 0 || file.size() > limit) {
        if (error) *error = QStringLiteral("Cannot safely read shared microG metadata: %1").arg(path);
        return false;
    }
    *contents = file.readAll();
    if (file.error() != QFileDevice::NoError || contents->size() > limit) {
        if (error) *error = QStringLiteral("Cannot read bounded shared microG metadata: %1").arg(path);
        return false;
    }
    return true;
}

bool writeAtomicFile(const QString &path, const QByteArray &contents, QString *error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size() ||
        !file.commit()) {
        if (error) *error = QStringLiteral("Cannot atomically write shared microG metadata: %1").arg(path);
        return false;
    }
    return true;
}
}

ManagerLifecycle::ManagerLifecycle(const std::function<bool()> &showGui, QObject *parent,
                                   int idleMilliseconds)
    : QObject(parent), m_showGui(showGui), m_provider(QDBusConnection::sessionBus(), nullptr,
                                                    [this]() { noteRequest(); })
{
    m_idle.setSingleShot(true);
    m_idle.setInterval(idleMilliseconds);
    connect(&m_idle, &QTimer::timeout, QCoreApplication::instance(), &QCoreApplication::quit);
    m_receiver.setActivationHandler([this]() { return this->showGui(); });
#ifdef ACL_STANDALONE_MICROG_DIAGNOSTIC
    connect(&m_receiver, &ExternalApkReceiver::diagnosticRequested, this, [this]() { noteRequest(); });
#endif
    connect(&m_receiver, &ExternalApkReceiver::requested, this, [this](const QString &path) {
        if (this->showGui()) emit fileRequested(path);
        else qWarning("Cannot display an incoming file request.");
    });
    connect(&m_receiver, &ExternalApkReceiver::rejected, this, [this](const QString &message) {
        emit fileRejected(message);
    });
    connect(&m_worker, &SharedMicrogSupervisor::workerDied,
            &m_provider, &SharedMicrogProviderObject::revokeAllChannels);
    m_microgRestart.setSingleShot(true);
    connect(&m_worker, &SharedMicrogSupervisor::workerDied, this, [this]() {
        if (m_microgBackground) m_microgRestart.start(30000);
    });
    connect(&m_microgRestart, &QTimer::timeout, this, [this]() {
        if (!m_microgBackground) return;
        QString error;
        if (!m_worker.startRetained(&error)) {
            qWarning("Cannot start the background microG worker: %s", qPrintable(error));
            if (m_microgBackground) m_microgRestart.start(30000);
        }
    });
    m_provider.setWorkerCheckHandler([this](QString *error) {
        if (!m_microgBackground) {
            if (error) *error = QStringLiteral("The shared microG worker is not configured.");
            return false;
        }
        if (m_microgRestart.isActive() && m_microgRestart.remainingTime() > 0) {
            if (error) *error = QStringLiteral("The shared microG worker is waiting to retry.");
            return false;
        }
        if (!m_worker.startRetained(error)) {
            if (m_microgBackground) m_microgRestart.start(30000);
            return false;
        }
        m_microgRestart.stop();
        return true;
    });
    connect(&m_worker, &SharedMicrogSupervisor::channelDisconnected,
            &m_provider, &SharedMicrogProviderObject::releaseChannelById);
    connect(&m_worker, &SharedMicrogSupervisor::authenticationUiChanged, this,
            [this](quint64 channelId, bool active) {
        if (active) {
            if (m_authUiChannel && m_authUiChannel != channelId) {
                m_worker.sendBack(channelId);
                return;
            }
            if (!this->showGui()) {
                m_worker.sendBack(channelId);
                return;
            }
            m_authUiChannel = channelId;
            m_authFrame = QImage();
            m_pendingAuthFrame = QImage();
            m_authViewport = QSize();
            m_authDensity = 0;
            m_authFrameCurrent = false;
            m_authViewportPending = false;
            m_authAwaitingViewportFrame = false;
            emit authenticationFrameChanged();
            emit authenticationUiChanged();
            return;
        }
        closeAuthenticationUi(channelId);
    });
    connect(&m_worker, &SharedMicrogSupervisor::authenticationFrameChanged, this,
            [this](quint64 channelId, const QImage &frame) {
        acceptAuthenticationFrame(channelId, frame);
    });
    connect(&m_worker, &SharedMicrogSupervisor::authenticationKeyboardRequested, this,
            [this](quint64 channelId, int kind, int flags, int enter) {
        if (m_authUiChannel == channelId && m_authKeyboard)
            m_authKeyboard->showKeyboard(kind, flags, enter);
    });
    connect(&m_worker, &SharedMicrogSupervisor::authenticationKeyboardHidden, this,
            [this](quint64 channelId) {
        if (m_authUiChannel == channelId && m_authKeyboard)
            m_authKeyboard->hideKeyboard();
    });
    connect(&m_worker, &SharedMicrogSupervisor::workerDied, this, [this]() {
        closeAuthenticationUi(m_authUiChannel);
    });
    connect(&m_worker, &SharedMicrogSupervisor::channelDisconnected, this,
            [this](quint64 channelId) { closeAuthenticationUi(channelId); });
}

void ManagerLifecycle::acceptAuthenticationFrame(quint64 channelId, const QImage &frame)
{
    if (m_authUiChannel != channelId) return;
    if (m_authViewportPending) {
        m_pendingAuthFrame = frame;
        return;
    }
    if (!m_authViewport.isEmpty() && frame.size() != m_authViewport) return;
    const bool firstFrame = m_authFrame.isNull() && !frame.isNull();
    m_authFrame = frame;
    m_authFrameCurrent = !frame.isNull();
    m_authAwaitingViewportFrame = false;
    if (firstFrame) {
        quint64 nonzero = 0;
        for (int row = 0; row < frame.height(); ++row) {
            const unsigned char *pixels = frame.constScanLine(row);
            for (int i = 0; i < frame.width() * 4; ++i)
                if (pixels[i]) ++nonzero;
        }
        qInfo("Shared microG UI frame ready: channel=%llu, size=%dx%d, nonzero_bytes=%llu",
              static_cast<unsigned long long>(channelId),
              frame.width(), frame.height(), static_cast<unsigned long long>(nonzero));
    }
    if (m_authKeyboard && m_authSurface)
        m_authKeyboard->setSurface(m_authSurface, frame.size());
    emit authenticationFrameChanged();
}

ManagerLifecycle::~ManagerLifecycle()
{
    if (m_authKeyboard) m_authKeyboard->stop();
    m_provider.revokeAllChannels();
    m_worker.stop();
}

void ManagerLifecycle::closeAuthenticationUi(quint64 channelId)
{
    if (!channelId || m_authUiChannel != channelId) return;
    m_authUiChannel = 0;
    m_authFrame = QImage();
    m_pendingAuthFrame = QImage();
    m_authViewport = QSize();
    m_authDensity = 0;
    m_authFrameCurrent = false;
    m_authViewportPending = false;
    m_authAwaitingViewportFrame = false;
    if (m_authKeyboard) m_authKeyboard->hideKeyboard();
    if (m_authKeyboard && m_authSurface)
        m_authKeyboard->setSurface(nullptr, QSize());
    m_authSurface.clear();
    emit authenticationFrameChanged();
    emit authenticationUiChanged();
}

void ManagerLifecycle::setAuthenticationKeyboard(PreviewKeyboard *keyboard)
{
    if (m_authKeyboard == keyboard) return;
    if (m_authKeyboard) {
        disconnect(m_authKeyboard, nullptr, this, nullptr);
        m_authKeyboard->stop();
    }
    m_authKeyboard = keyboard;
    if (!keyboard) return;
    keyboard->setRemote(true);
    keyboard->start();
    connect(keyboard, &PreviewKeyboard::textCommitted, this,
            [this](const QString &text) { sendAuthenticationText(text); });
    connect(keyboard, &PreviewKeyboard::textKey, this,
            [this](int key) { sendAuthenticationTextKey(key); });
    connect(keyboard, &PreviewKeyboard::keyboardClosed, this,
            [this]() { sendAuthenticationKeyboardClosed(); });
    connect(keyboard, &PreviewKeyboard::keyboardHeight, this,
            [this](int height) { sendAuthenticationKeyboardHeight(height); });
}

bool ManagerLifecycle::authenticationTouch(int phase, int x, int y)
{
    return m_authUiChannel && m_authFrameCurrent &&
           m_worker.sendTouch(m_authUiChannel, phase, 0, x, y);
}

void ManagerLifecycle::authenticationBack()
{
    if (!m_authUiChannel) return;
    if (m_authKeyboard && m_authKeyboard->back()) return;
    m_worker.sendBack(m_authUiChannel);
}

void ManagerLifecycle::cancelAuthentication()
{
    if (m_authUiChannel) m_worker.sendBack(m_authUiChannel);
}

void ManagerLifecycle::setAuthenticationSurface(QObject *surface, int pixelWidth, int pixelHeight)
{
    QQuickItem *item = qobject_cast<QQuickItem *>(surface);
    m_authSurface = item;
    if (m_authKeyboard)
        m_authKeyboard->setSurface(item, m_authFrame.isNull()
                                   ? QSize(qMax(0, pixelWidth), qMax(0, pixelHeight))
                                   : m_authFrame.size());
}

void ManagerLifecycle::setAuthenticationViewport(const QSize &pixels, int density, int rotation)
{
    if (!m_authUiChannel || pixels.isEmpty()) return;
    if (m_authViewport == pixels && m_authDensity == density && m_authRotation == rotation) {
        m_authViewportPending = false;
        const QImage pending = m_pendingAuthFrame;
        m_pendingAuthFrame = QImage();
        if (!m_authAwaitingViewportFrame && !pending.isNull() && pending.size() == pixels) {
            acceptAuthenticationFrame(m_authUiChannel, pending);
            return;
        }
        m_authFrameCurrent = !m_authAwaitingViewportFrame && !m_authFrame.isNull() &&
                             m_authFrame.size() == pixels;
        return;
    }
    const bool reuseInitial = !m_authDensity && !m_pendingAuthFrame.isNull() &&
                              m_pendingAuthFrame.size() == pixels;
    const bool mustWait = m_authFrame.isNull() || m_authFrame.size() != pixels ||
                          (m_authDensity && (m_authDensity != density || m_authRotation != rotation));
    if (mustWait) {
        m_authFrameCurrent = false;
        m_authAwaitingViewportFrame = true;
    }
    if (!m_worker.sendUiResize(m_authUiChannel, pixels.width(), pixels.height(), density, rotation)) {
        m_authFrameCurrent = false;
        qWarning("Shared microG UI viewport update failed.");
        return;
    }
    m_authViewport = pixels;
    m_authDensity = density;
    m_authRotation = rotation;
    m_authViewportPending = false;
    const QImage pending = m_pendingAuthFrame;
    m_pendingAuthFrame = QImage();
    if (reuseInitial) acceptAuthenticationFrame(m_authUiChannel, pending);
}

void ManagerLifecycle::invalidateAuthenticationViewport()
{
    if (!m_authUiChannel) return;
    m_authViewportPending = true;
    m_authFrameCurrent = false;
}

bool ManagerLifecycle::sendAuthenticationText(const QString &text)
{
    if (!m_authUiChannel) return false;
    return m_worker.sendText(m_authUiChannel, text.toUtf8());
}

bool ManagerLifecycle::sendAuthenticationTextKey(int key)
{
    return m_authUiChannel && m_worker.sendTextKey(m_authUiChannel, key);
}

void ManagerLifecycle::sendAuthenticationKeyboardClosed()
{
    if (m_authUiChannel) m_worker.sendKeyboardClosed(m_authUiChannel);
}

void ManagerLifecycle::sendAuthenticationKeyboardHeight(int height)
{
    if (m_authUiChannel) m_worker.sendKeyboardHeight(m_authUiChannel, height);
}

bool ManagerLifecycle::configureSharedMicrogWorker(const QString &program,
                                                   const QStringList &arguments,
                                                   const SharedMicrogProviderObject::PackageResolver &packageResolver,
                                                   QString *error)
{
    return configureSharedMicrogWorker(program, arguments, packageResolver,
                                       SharedMicrogProviderObject::ApkMetadataResolver(), error);
}

bool ManagerLifecycle::configureSharedMicrogWorker(const QString &program,
                                                   const QStringList &arguments,
                                                   const SharedMicrogProviderObject::PackageResolver &packageResolver,
                                                   const SharedMicrogProviderObject::ApkMetadataResolver &metadataResolver,
                                                   QString *error)
{
    if (!packageResolver) {
        if (error) *error = QStringLiteral("A trusted shared microG package resolver is required.");
        return false;
    }
    if (!m_worker.configure(program, arguments, error)) return false;
    if (!m_provider.setPackageResolver(packageResolver, error)) return false;
    if (metadataResolver && !m_provider.setApkMetadataResolver(metadataResolver, error)) return false;
    m_appsDirectory.clear();
    for (const QString &argument : arguments) {
        if (argument.startsWith(QStringLiteral("--apps-dir="))) {
            m_appsDirectory = argument.mid(QStringLiteral("--apps-dir=").size());
            break;
        }
    }
    const SharedMicrogProviderObject::ChannelHandler open =
        [this](const QString &, const QString &package, const QByteArray &appMetadata,
               int uid, int pid, quint64 channelId,
               const QDBusUnixFileDescriptor &server, QString *workerError) {
            if (!server.isValid()) {
                if (workerError) *workerError = QStringLiteral("Invalid shared microG stream.");
                return false;
            }
            if (!appMetadata.isEmpty() &&
                !publishClientAppMetadata(package, appMetadata, channelId, workerError))
                return false;
            if (m_worker.openChannel(package, uid, pid, channelId,
                                     server.fileDescriptor(), workerError))
                return true;
            releaseClientAppMetadata(channelId);
            return false;
        };
    const SharedMicrogProviderObject::ChannelReleaseHandler close =
        [this](const QString &, quint64 channelId) {
            m_worker.closeChannel(channelId);
            releaseClientAppMetadata(channelId);
        };
    return m_provider.setChannelHandlers(open, close, error);
}

bool ManagerLifecycle::publishClientAppMetadata(const QString &package, const QByteArray &payload,
                                               quint64 channelId, QString *error)
{
    if (error) error->clear();
    static const QRegularExpression packagePattern(QStringLiteral(
        "\\A[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)+\\z"));
    if (m_appsDirectory.isEmpty() || !packagePattern.match(package).hasMatch() ||
        payload.isEmpty() || payload.size() > maxAppMetadataBytes || m_clientAppChannels.contains(channelId)) {
        if (error) *error = QStringLiteral("Invalid or oversized shared microG client metadata.");
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = QStringLiteral("The shared microG client metadata is malformed.");
        return false;
    }
    const QJsonObject metadata = document.object();
    auto safeField = [](QString value) {
        value.replace(QLatin1Char('\t'), QLatin1Char(' '));
        value.replace(QLatin1Char('\r'), QLatin1Char(' '));
        value.replace(QLatin1Char('\n'), QLatin1Char(' '));
        return value;
    };
    const QString activity = safeField(metadata.value(QStringLiteral("activity")).toString());
    const QString label = safeField(metadata.value(QStringLiteral("label")).toString());
    const QString version = safeField(metadata.value(QStringLiteral("versionName")).toString());
    const QString versionCode = metadata.value(QStringLiteral("versionCode")).toString();
    const QJsonValue activitiesValue = metadata.value(QStringLiteral("activities"));
    if (metadata.value(QStringLiteral("package")).toString() != package ||
        activity.size() > 200 || label.size() > 1024 || version.size() > 256 ||
        versionCode.size() > 32 || !QRegularExpression(QStringLiteral("\\A[0-9]+\\z"))
            .match(versionCode).hasMatch() || !activitiesValue.isObject()) {
        if (error) *error = QStringLiteral("The shared microG client metadata does not match the caller.");
        return false;
    }
    const QByteArray activities = QJsonDocument(activitiesValue.toObject()).toJson(QJsonDocument::Compact);
    if (activities.isEmpty() || activities.size() > maxAppMetadataBytes) {
        if (error) *error = QStringLiteral("The shared microG APK activities exceed the metadata limit.");
        return false;
    }
    const QJsonObject activityInfo = activitiesValue.toObject();
    const QJsonArray certificates = activityInfo.value(QStringLiteral("certificates")).toArray();
    if (activityInfo.value(QStringLiteral("package")).toString() != package ||
        certificates.isEmpty() || certificates.size() > 8) {
        if (error) *error = QStringLiteral("The shared microG APK description is invalid.");
        return false;
    }
    for (const QJsonValue &certificate : certificates) {
        const QString hex = certificate.toString();
        if (certificate.type() != QJsonValue::String || hex.isEmpty() || hex.size() > 32768 ||
            (hex.size() & 1) || !QRegularExpression(QStringLiteral("\\A[0-9A-Fa-f]+\\z"))
                .match(hex).hasMatch()) {
            if (error) *error = QStringLiteral("The shared microG APK certificate data is invalid.");
            return false;
        }
    }

    const QByteArray appsLine = (package + QLatin1Char('\t') + activity + QLatin1Char('\t') +
                                 label + QLatin1Char('\t') + version + QLatin1Char('\t') +
                                 versionCode + QLatin1Char('\n')).toUtf8();
    auto existing = m_clientAppMetadata.find(package);
    if (existing != m_clientAppMetadata.end()) {
        if (existing->payload != payload) {
            if (error) *error = QStringLiteral("A different APK version of this package is already connected to shared microG.");
            return false;
        }
        existing->channels.insert(channelId);
        m_clientAppChannels.insert(channelId, package);
        return true;
    }

    if (QFileInfo(m_appsDirectory).isSymLink() || !QDir().mkpath(m_appsDirectory)) {
        if (error) *error = QStringLiteral("The shared microG apps directory is unavailable.");
        return false;
    }
    ClientAppMetadata state;
    state.payload = payload;
    state.appsLine = appsLine;
    const QString appsPath = QDir(m_appsDirectory).filePath(QStringLiteral("apps.txt"));
    QByteArray appsText;
    if (!readBoundedFile(appsPath, maxAppsListBytes, &appsText, &state.hadAppsFile, error))
        return false;
    const QList<QByteArray> lines = appsText.split('\n');
    for (const QByteArray &line : lines) {
        if (line.left(line.indexOf('\t') < 0 ? line.size() : line.indexOf('\t')) == package.toUtf8())
            state.previousAppsLines.append(line + (line.isEmpty() ? QByteArray() : QByteArray("\n")));
    }
    QByteArray previousActivities;
    const QString packageDirectory = QDir(m_appsDirectory).filePath(package);
    const QString activitiesPath = QDir(packageDirectory).filePath(QStringLiteral("activities.json"));
    if (QFileInfo(packageDirectory).isSymLink() ||
        (QFileInfo(packageDirectory).exists() && !QFileInfo(packageDirectory).isDir()) ||
        !readBoundedFile(activitiesPath, maxAppMetadataBytes, &previousActivities,
                         &state.hadActivities, error)) {
        if (error && error->isEmpty())
            *error = QStringLiteral("The shared microG package metadata path is unsafe.");
        return false;
    }
    state.previousActivities = previousActivities;
    if (!QDir().mkpath(packageDirectory) ||
        !writeAtomicFile(activitiesPath, activities, error)) return false;

    state.channels.insert(channelId);
    m_clientAppMetadata.insert(package, state);
    m_clientAppChannels.insert(channelId, package);
    if (!writeClientAppsList(error)) {
        m_clientAppChannels.remove(channelId);
        m_clientAppMetadata.remove(package);
        if (state.hadActivities)
            writeAtomicFile(activitiesPath, state.previousActivities, nullptr);
        else if (QFileInfo::exists(activitiesPath) && !QFile::remove(activitiesPath))
            qWarning("Cannot remove rejected shared microG APK metadata: %s", qPrintable(activitiesPath));
        return false;
    }
    return true;
}

bool ManagerLifecycle::writeClientAppsList(QString *error)
{
    if (m_appsDirectory.isEmpty() || QFileInfo(m_appsDirectory).isSymLink()) {
        if (error) *error = QStringLiteral("The shared microG apps directory is unavailable.");
        return false;
    }
    const QString path = QDir(m_appsDirectory).filePath(QStringLiteral("apps.txt"));
    QByteArray current;
    bool exists = false;
    if (!readBoundedFile(path, maxAppsListBytes, &current, &exists, error)) return false;
    QByteArray updated;
    const QList<QByteArray> lines = current.split('\n');
    for (const QByteArray &line : lines) {
        if (line.isEmpty()) continue;
        const int tab = line.indexOf('\t');
        const QByteArray package = line.left(tab < 0 ? line.size() : tab);
        if (!m_clientAppMetadata.contains(QString::fromUtf8(package)))
            updated += line + '\n';
    }
    for (auto it = m_clientAppMetadata.constBegin(); it != m_clientAppMetadata.constEnd(); ++it)
        updated += it->appsLine;
    if (updated.size() > maxAppsListBytes) {
        if (error) *error = QStringLiteral("The shared microG apps list exceeds the metadata limit.");
        return false;
    }
    if (updated.isEmpty() && !exists) return true;
    return writeAtomicFile(path, updated, error);
}

void ManagerLifecycle::releaseClientAppMetadata(quint64 channelId)
{
    const auto channel = m_clientAppChannels.find(channelId);
    if (channel == m_clientAppChannels.end()) return;
    const QString package = channel.value();
    m_clientAppChannels.erase(channel);
    auto state = m_clientAppMetadata.find(package);
    if (state == m_clientAppMetadata.end()) return;
    state->channels.remove(channelId);
    if (!state->channels.isEmpty()) return;

    const ClientAppMetadata previous = state.value();
    const QString appsPath = QDir(m_appsDirectory).filePath(QStringLiteral("apps.txt"));
    QByteArray appsText;
    bool exists = false;
    QString error;
    if (readBoundedFile(appsPath, maxAppsListBytes, &appsText, &exists, &error)) {
        const QList<QByteArray> lines = appsText.split('\n');
        QByteArray restored;
        bool replacedInjectedLine = false;
        for (const QByteArray &line : lines) {
            if (line.isEmpty()) continue;
            if (line + '\n' == previous.appsLine) {
                if (!replacedInjectedLine) {
                    for (const QByteArray &oldLine : previous.previousAppsLines)
                        restored += oldLine;
                    replacedInjectedLine = true;
                }
            } else {
                restored += line + '\n';
            }
        }
        if (replacedInjectedLine && restored.isEmpty() && !previous.hadAppsFile) {
            if (QFileInfo::exists(appsPath) && !QFile::remove(appsPath))
                qWarning("Cannot remove temporary shared microG apps list: %s", qPrintable(appsPath));
        } else if ((!restored.isEmpty() || exists) && !writeAtomicFile(appsPath, restored, &error)) {
            qWarning("Cannot restore shared microG apps list: %s", qPrintable(error));
        }
    } else {
        qWarning("Cannot restore shared microG apps list: %s", qPrintable(error));
    }

    const QString activitiesPath = QDir(m_appsDirectory).filePath(
        package + QStringLiteral("/activities.json"));
    QByteArray activities;
    bool activitiesExist = false;
    if (!QFileInfo(QDir(m_appsDirectory).filePath(package)).isSymLink() &&
        readBoundedFile(activitiesPath, maxAppMetadataBytes, &activities,
                        &activitiesExist, &error) &&
        activitiesExist) {
        const QJsonObject metadata = QJsonDocument::fromJson(previous.payload).object();
        const QByteArray injected = QJsonDocument(metadata.value(QStringLiteral("activities")).toObject())
                                       .toJson(QJsonDocument::Compact);
        if (activities == injected) {
            if (previous.hadActivities) {
                if (!writeAtomicFile(activitiesPath, previous.previousActivities, &error))
                    qWarning("Cannot restore shared microG APK description: %s", qPrintable(error));
            } else if (!QFile::remove(activitiesPath)) {
                qWarning("Cannot remove temporary shared microG APK description: %s", qPrintable(activitiesPath));
            }
        }
    } else if (!error.isEmpty()) {
        qWarning("Cannot restore shared microG APK description: %s", qPrintable(error));
    }
    m_clientAppMetadata.erase(state);
}

void ManagerLifecycle::disableSharedMicrogWorker()
{
    m_microgBackground = false;
    m_microgRestart.stop();
    m_worker.releaseRetention();
    m_provider.revokeAllChannels();
    m_worker.stop();
    QString error;
    if (!m_provider.clearBackend(&error))
        qWarning("Cannot disable shared microG backend: %s", qPrintable(error));
}

void ManagerLifecycle::enableSharedMicrogBackground()
{
    m_microgBackground = true;
    m_microgRestart.start(0);
}

SharedMicrogProviderObject::PackageResolver ManagerLifecycle::registryPackageResolver(
    const QString &identityDirectory)
{
    return [identityDirectory](const QString &executable, const QString &identityKey, QString *error) {
        if (error) error->clear();
        if (identityKey.isEmpty()) {
            if (error) *error = QStringLiteral("Caller identity is incomplete.");
            return QString();
        }
        const QString appId = StandaloneDiagnosticRegistry::appIdFromExecutable(executable);
        if (appId.isEmpty()) {
            if (error) *error = QStringLiteral("Caller is not a registered standalone app.");
            return QString();
        }
        const QString package = StandaloneDiagnosticRegistry::lookup(identityDirectory, appId);
        if (package.isEmpty() && error)
            *error = QStringLiteral("No exported package mapping exists for this caller.");
        return package;
    };
}

bool ManagerLifecycle::showGui()
{
    if (!m_showGui || !m_showGui()) return false;
    m_background = false;
    m_idle.stop();
    return true;
}

void ManagerLifecycle::noteRequest()
{
    if (m_background && !m_keepAlive) m_idle.start();
}

void ManagerLifecycle::setKeepAlive(bool keepAlive)
{
    m_keepAlive = keepAlive;
    if (keepAlive) m_idle.stop();
    else if (m_background) m_idle.start();
}

bool ManagerLifecycle::start(bool background, QString *error)
{
    if (error) error->clear();
    if (!m_receiver.start(error)) return false;
    if (!m_receiver.ownsLegacyService()) {
        if (background) {
            if (error) *error = QStringLiteral("The manager D-Bus service is unavailable.");
            return false;
        }
        qWarning("Shared microG provider service is unavailable: manager D-Bus name not owned.");
        return true;
    }

    QString providerError;
    if (!m_provider.start(&providerError)) {
        if (background) {
            if (error) *error = providerError;
            return false;
        }
        qWarning("Shared microG provider service is unavailable: %s", qPrintable(providerError));
    }
    m_background = background;
    if (background && !m_keepAlive) m_idle.start();
    return true;
}

SharedMicrogAuthSurface::SharedMicrogAuthSurface(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    setAcceptedMouseButtons(Qt::LeftButton);
    setFlag(ItemHasContents, true);
    m_viewportTimer.setSingleShot(true);
    connect(&m_viewportTimer, &QTimer::timeout, this, &SharedMicrogAuthSurface::updateViewport);
    connect(this, &QQuickItem::windowChanged, this, [this](QQuickWindow *window) {
        disconnect(m_screenConnection);
        if (window)
            m_screenConnection = connect(window, &QQuickWindow::screenChanged,
                                         this, [this](QScreen *) { scheduleViewport(); });
        scheduleViewport();
    });
}

void SharedMicrogAuthSurface::setHost(QObject *host)
{
    ManagerLifecycle *lifecycle = qobject_cast<ManagerLifecycle *>(host);
    if (m_host == lifecycle) return;
    if (m_host) disconnect(m_host, nullptr, this, nullptr);
    m_host = lifecycle;
    if (m_host) {
        connect(m_host, &ManagerLifecycle::authenticationFrameChanged, this,
                [this]() { update(); });
        connect(m_host, &ManagerLifecycle::authenticationUiChanged, this,
                [this]() { update(); scheduleViewport(); });
    }
    update();
    scheduleViewport();
    emit hostChanged();
}

void SharedMicrogAuthSurface::setPageOrientation(int orientation)
{
    if (m_pageOrientation == orientation) return;
    m_pageOrientation = orientation;
    scheduleViewport();
}

void SharedMicrogAuthSurface::geometryChanged(const QRectF &geometry, const QRectF &oldGeometry)
{
    QQuickPaintedItem::geometryChanged(geometry, oldGeometry);
    if (geometry.size() != oldGeometry.size()) scheduleViewport();
}

void SharedMicrogAuthSurface::scheduleViewport()
{
    if (m_pressed && m_host) {
        m_host->authenticationTouch(2, m_lastGuestX, m_lastGuestY);
        m_pressed = false;
    }
    if (m_host && window() && width() > 0 && height() > 0)
        m_host->invalidateAuthenticationViewport();
    m_viewportTimer.start(0);
}

void SharedMicrogAuthSurface::updateViewport()
{
    if (!m_host || !window() || width() <= 0 || height() <= 0) return;
    const PreviewViewport viewport = previewViewport(width(), height(), window()->devicePixelRatio(), 2048);
    QScreen *screen = window()->screen();
    if (screen) {
        const Qt::ScreenOrientation native = previewNativeOrientation(
            screen->nativeOrientation(), QGuiApplication::platformName().startsWith(QLatin1String("wayland")));
        const int rotation = previewDisplayRotation(native, static_cast<Qt::ScreenOrientation>(m_pageOrientation));
        if (rotation >= 0) m_rotation = rotation;
    }
    m_host->setAuthenticationViewport(viewport.pixels, viewport.density, m_rotation);
}

void SharedMicrogAuthSurface::paint(QPainter *painter)
{
    painter->fillRect(boundingRect(), Qt::black);
    if (!m_host) return;
    const QImage frame = m_host->authenticationFrame();
    if (frame.isNull()) return;
    painter->drawImage(imageTarget(frame), frame);
}

QRectF SharedMicrogAuthSurface::imageTarget(const QImage &frame) const
{
    if (frame.isNull() || width() <= 0 || height() <= 0) return QRectF();
    const QSizeF scaled = QSizeF(frame.size()).scaled(QSizeF(width(), height()), Qt::KeepAspectRatio);
    return QRectF((width() - scaled.width()) / 2, (height() - scaled.height()) / 2,
                  scaled.width(), scaled.height());
}

bool SharedMicrogAuthSurface::sendTouch(int phase, const QPointF &point)
{
    if (!m_host) return false;
    const QImage frame = m_host->authenticationFrame();
    if (frame.isNull()) return false;
    const QRectF target = imageTarget(frame);
    if (target.isEmpty() || (phase == 0 && !target.contains(point))) return false;
    const int x = qBound(0, qRound((point.x() - target.left()) * frame.width() / target.width()),
                         frame.width() - 1);
    const int y = qBound(0, qRound((point.y() - target.top()) * frame.height() / target.height()),
                         frame.height() - 1);
    if (!m_host->authenticationTouch(phase, x, y)) return false;
    m_lastGuestX = x;
    m_lastGuestY = y;
    return true;
}

void SharedMicrogAuthSurface::mousePressEvent(QMouseEvent *event)
{
    m_lastTouch = event->localPos();
    m_pressed = event->button() == Qt::LeftButton && sendTouch(0, m_lastTouch);
    event->setAccepted(m_pressed);
}

void SharedMicrogAuthSurface::mouseMoveEvent(QMouseEvent *event)
{
    m_lastTouch = event->localPos();
    event->setAccepted(m_pressed && sendTouch(1, m_lastTouch));
}

void SharedMicrogAuthSurface::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_pressed) sendTouch(2, event->localPos());
    m_pressed = false;
    event->accept();
}

void SharedMicrogAuthSurface::mouseUngrabEvent()
{
    if (m_pressed) sendTouch(2, m_lastTouch);
    m_pressed = false;
}

QStringList ManagerLifecycle::launchFileArguments(const QStringList &arguments)
{
    // Aurora debug launches can leave the desktop Exec field code unexpanded.
    if (arguments.size() == 1 && arguments.first() == QLatin1String("%u"))
        return QStringList();
    return arguments;
}

ManagerLifecycle::ForwardResult ManagerLifecycle::forwardToRunningManager(
    const QStringList &arguments, QString *error)
{
    if (error) error->clear();
    const QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected() || !bus.interface()) {
        if (error) *error = QStringLiteral("Session D-Bus is unavailable.");
        return Unavailable;
    }
    const QDBusReply<bool> registered = bus.interface()->isServiceRegistered(QString::fromLatin1(ServiceName));
    if (!registered.isValid()) {
        if (error) *error = registered.error().message();
        return Unavailable;
    }
    if (!registered.value()) return NoOwner;
    QDBusMessage message = QDBusMessage::createMethodCall(
        QString::fromLatin1(ServiceName), QString::fromLatin1(ObjectPath),
        QString::fromLatin1(ServiceName), QStringLiteral("activate"));
    message.setAutoStartService(false);
    message << arguments;
    const QDBusReply<bool> result = bus.call(message, QDBus::Block, 5000);
    if (!result.isValid()) {
        if (error) *error = result.error().name() + QStringLiteral(": ") + result.error().message();
        return Failed;
    }
    if (!result.value()) {
        if (error) *error = QStringLiteral("The running manager could not open its window.");
        return Failed;
    }
    return Delivered;
}
