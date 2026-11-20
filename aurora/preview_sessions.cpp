#include "application_executables.h"
#include "preview_sessions.h"

#include "install_request_coordinator.h"
#include "log_channel.h"
#include "diagnostic_log.h"
#include "error_log.h"
#include "preview_activation_channel.h"

#include <QCoreApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTimer>
#include <QUuid>

class PreviewSession : public QObject
{
public:
    PreviewSession(const QString &name, const QString &data, const QString &store)
        : key(name), instance(QUuid::createUuid().toString()), install(&process, data, store),
          activation(&process) {}

    QString key;
    QString instance;
    QProcess process;
    InstallRequestCoordinator install;
    PreviewActivationServer activation;
    LogChannel log;
    QString token;
    quint64 request = 0;
    bool active = true;
    bool killRequested = false;
    qint64 processId = 0;
};

PreviewSessions::PreviewSessions(QObject *parent) : QObject(parent) {}

void PreviewSessions::setDiagnosticLog(DiagnosticLog *log) { m_diagnosticLog = log; }

PreviewSessions::~PreviewSessions()
{
    for (PreviewSession *session : m_sessions) {
        if (session->active)
            qWarning("PreviewSessions destroyed while an Android app is running.");
        delete session;
    }
}

bool PreviewSessions::start(const QString &key, const QString &androidRoot, const QString &data,
                            const QString &apk, const QString &storeRoot, QString *error,
                            EmbeddedEndpoint *embedded)
{
    if (error) error->clear();
    if (key.isEmpty() || androidRoot.isEmpty() || data.isEmpty() || storeRoot.isEmpty()) {
        if (error) *error = tr("Cannot start an app without its storage paths.");
        return false;
    }
    if (running(key)) {
        if (error) *error = tr("This Android app is already running.");
        return false;
    }
    PreviewSession *session = new PreviewSession(key, data, storeRoot);
    QString channelError;
    if (!session->install.listen(&channelError)) {
        if (error) *error = tr("Cannot start the installation channel: %1").arg(channelError);
        delete session;
        return false;
    }
    if (!session->activation.listen(&channelError)) {
        if (error) *error = tr("Cannot start the preview activation channel: %1").arg(channelError);
        delete session;
        return false;
    }
    if (embedded && !embedded->listen(&session->process, &channelError)) {
        if (error) *error = tr("Cannot start the embedded preview channel: %1").arg(channelError);
        delete session;
        return false;
    }
    m_sessions.append(session);
    if (!session->log.available()) emit logUnavailable(key);
    connect(&session->log, &LogChannel::changed, this, [this, session]() {
        emit logChanged(session->instance, session->key, session->log.text());
    });
    connect(&session->install, &InstallRequestCoordinator::installed, this, &PreviewSessions::installed);
    connect(&session->activation, &PreviewActivationServer::completed, this,
            [this, session](bool activated, const QString &error) {
        emit activationFinished(session->instance, session->key, activated, error);
    });
    connect(&session->install, &InstallRequestCoordinator::confirmationRequested, this,
            [this, session](quint64 id, const QString &label, const QString &package, quint32 version) {
        session->request = id;
        session->token = QUuid::createUuid().toString();
        emit confirmationRequested(session->token, label, package, version);
    });
    connect(&session->install, &InstallRequestCoordinator::confirmationCanceled, this,
            [this, session](quint64 id) {
        if (session->request != id) return;
        const QString token = session->token;
        session->request = 0;
        session->token.clear();
        emit confirmationCanceled(token);
    });
    if (m_diagnosticLog && m_diagnosticLog->available()) {
        session->process.setProcessChannelMode(QProcess::SeparateChannels);
        m_diagnosticLog->watch(&session->process, key + QLatin1Char(' ') + session->instance);
    } else {
        session->process.setProcessChannelMode(QProcess::ForwardedChannels);
    }
    connect(&session->process, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, [this, session](int code, QProcess::ExitStatus status) {
        finish(session, code, status == QProcess::CrashExit, QString());
    });
    connect(&session->process, &QProcess::errorOccurred, this, [this, session](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish(session, -1, false, session->process.errorString());
    });
    connect(&session->process, &QProcess::started, this, [this, session]() {
        session->processId = session->process.processId();
        ErrorLog::record(QStringLiteral("Lifecycle"),
                         QStringLiteral("Preview process: pid=%1 started").arg(session->processId), false);
        if (session->killRequested) session->process.kill();
        else emit started(session->key);
    });
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    if ((!m_diagnosticLog || !m_diagnosticLog->available()) && session->log.available())
        environment.insert(QStringLiteral("ANDROID_COMPATIBILITY_LAYER_CORE_LOG_SOCKET"), session->log.path());
    else
        environment.remove(QStringLiteral("ANDROID_COMPATIBILITY_LAYER_CORE_LOG_SOCKET"));
    session->process.setProcessEnvironment(environment);
    QStringList arguments = QStringList() << QStringLiteral("--preview") << androidRoot << data << apk
                                          << session->install.endpoint() << session->install.resultsDir()
                                          << session->activation.endpoint();
    if (embedded) arguments << embedded->endpoint();
    session->process.start(ApplicationExecutables::worker(), arguments);
    emit runningChanged();
    return true;
}

bool PreviewSessions::activate(const QString &key, int timeoutMs, QString *error)
{
    if (error) error->clear();
    for (PreviewSession *session : m_sessions) {
        if (session->active && session->key == key)
            return session->activation.request(timeoutMs, error);
    }
    if (error) *error = tr("This Android app is not running.");
    return false;
}

bool PreviewSessions::running(const QString &key) const
{
    for (const PreviewSession *session : m_sessions)
        if (session->active && session->key == key) return true;
    return false;
}

bool PreviewSessions::kill(const QString &key)
{
    for (PreviewSession *session : m_sessions) {
        if (!session->active || session->key != key) continue;
        if (session->process.state() == QProcess::Starting) {
            session->killRequested = true;
            return true;
        }
        if (session->process.state() == QProcess::Running) {
            session->killRequested = true;
            session->process.kill();
            return true;
        }
    }
    return false;
}

int PreviewSessions::count() const
{
    int result = 0;
    for (const PreviewSession *session : m_sessions)
        if (session->active) ++result;
    return result;
}

PreviewSession *PreviewSessions::pending(const QString &token) const
{
    if (token.isEmpty()) return nullptr;
    for (PreviewSession *session : m_sessions)
        if (session->active && session->token == token && session->request) return session;
    return nullptr;
}

void PreviewSessions::decision(const QString &token, bool accepted)
{
    PreviewSession *session = pending(token);
    if (!session) return;
    const quint64 id = session->request;
    session->request = 0;
    session->token.clear();
    session->install.decision(id, accepted);
}

void PreviewSessions::unavailable(const QString &token)
{
    PreviewSession *session = pending(token);
    if (!session) return;
    const quint64 id = session->request;
    session->request = 0;
    session->token.clear();
    session->install.unavailable(id);
}

void PreviewSessions::finish(PreviewSession *session, int code, bool crashed, const QString &error)
{
    if (!session->active) return;
    session->active = false;
    QString event;
    if (!session->processId) {
        event = QStringLiteral("Preview process: result=failed-to-start");
    } else {
        const char *result = session->killRequested && crashed ? "stopped"
            : crashed ? "crashed" : code != 0 ? "nonzero" : "completed";
        event = QStringLiteral("Preview process: pid=%1 result=%2")
            .arg(session->processId).arg(QLatin1String(result));
    }
    ErrorLog::record(QStringLiteral("Lifecycle"), event, false);
    if (!session->token.isEmpty()) {
        const QString token = session->token;
        session->token.clear();
        session->request = 0;
        emit confirmationCanceled(token);
    }
    emit runningChanged();
    emit finished(session->instance, session->key, code, crashed, error);
    QTimer::singleShot(200, this, [this, session]() {
        emit logChanged(session->instance, session->key, session->log.text());
        m_sessions.removeOne(session);
        delete session;
    });
}
