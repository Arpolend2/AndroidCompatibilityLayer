#include "store_preview_supervisor.h"
#include "embedded_preview.h"

#include <QLocalSocket>
#include <QProcessEnvironment>
#include <QTimer>
#include <sys/socket.h>

StorePreviewSupervisor::StorePreviewSupervisor(EmbeddedPreview *preview, const QString &program,
                                               const QStringList &arguments, QObject *parent)
    : QObject(parent), m_preview(preview), m_program(program), m_arguments(arguments)
{
    connect(&m_server, &QLocalServer::newConnection, this, &StorePreviewSupervisor::acceptPending);
    connect(m_preview, &EmbeddedPreview::shutdownComplete, this, [this]() {
        if (!m_process || !m_preview->stopping() || m_stopTeardownWatchdogArmed) return;
        m_stopTeardownWatchdogArmed = true;
        QPointer<QProcess> process = m_process;
        QTimer::singleShot(500, Qt::PreciseTimer, this, [this, process]() {
            if (!process || process != m_process || process->state() == QProcess::NotRunning) return;
            qWarning("Aurora Store close: worker teardown did not finish in time; terminating pid=%lld.",
                     static_cast<long long>(process->processId()));
            process->kill();
        });
    });
    m_clock.start();
}

bool StorePreviewSupervisor::start(QString *error)
{
    if (error) error->clear();
    if (!m_directory.isValid() || !m_server.listen(m_directory.path() + QStringLiteral("/restart"))) {
        if (error) *error = tr("Cannot initialize the store restart channel: %1").arg(m_server.errorString());
        return false;
    }
    return launch(error);
}

bool StorePreviewSupervisor::launch(QString *error)
{
    QProcess *process = new QProcess(this);
    if (!m_preview->listen(process, error)) {
        delete process;
        return false;
    }
    m_process = process;
    m_requested = false;
    m_restartKillRequested = false;
    m_stopTeardownWatchdogArmed = false;
    process->setProcessChannelMode(QProcess::ForwardedChannels);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("ANDROID_COMPATIBILITY_LAYER_STORE_RESTART_SOCKET"), m_server.fullServerName());
    process->setProcessEnvironment(env);
    connect(process, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, [this, process](int code, QProcess::ExitStatus status) {
        if (m_process != process) return;
        const bool restart = m_requested && !m_stopping && !m_preview->stopping() &&
                ((status == QProcess::NormalExit && code == 0) ||
                 (status == QProcess::CrashExit && m_restartKillRequested));
        qInfo("Aurora Store worker finished: code=%d crashed=%d restart=%d watchdog=%d",
              code, status == QProcess::CrashExit, restart, m_restartKillRequested);
        m_process.clear();
        process->deleteLater();
        if (restart) {
            QString error;
            if (launch(&error)) return;
            qWarning("Cannot restart Aurora Store: %s", qPrintable(error));
            emit finished(1, false);
            return;
        }
        emit finished(code, status == QProcess::CrashExit);
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (m_process != process || error != QProcess::FailedToStart) return;
        qWarning("Cannot start Aurora Store: %s", qPrintable(process->errorString()));
        m_process.clear();
        process->deleteLater();
        emit finished(1, false);
    });
    process->start(m_program, m_arguments + QStringList(m_preview->endpoint()));
    return true;
}

void StorePreviewSupervisor::acceptPending()
{
    while (m_server.hasPendingConnections()) {
        QLocalSocket *socket = m_server.nextPendingConnection();
        struct ucred peer = {};
        socklen_t size = sizeof(peer);
        if (!m_process || m_process->processId() <= 0 ||
                getsockopt(int(socket->socketDescriptor()), SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 ||
                size != sizeof(peer) || peer.pid != m_process->processId()) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        socket->setParent(this);
        connect(socket, &QLocalSocket::readyRead, this, [this, socket]() { request(socket); });
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        QTimer::singleShot(2000, socket, [socket]() { socket->abort(); });
        if (socket->bytesAvailable()) request(socket);
    }
}

void StorePreviewSupervisor::request(QLocalSocket *socket)
{
    const QByteArray message = socket->readAll();
    struct ucred peer = {};
    socklen_t size = sizeof(peer);
    const bool current = m_process && m_process->processId() > 0 &&
            getsockopt(int(socket->socketDescriptor()), SOL_SOCKET, SO_PEERCRED, &peer, &size) == 0 &&
            size == sizeof(peer) && peer.pid == m_process->processId();
    const qint64 now = m_clock.elapsed();
    while (!m_restarts.isEmpty() && m_restarts.first() <= now - 60000) m_restarts.removeFirst();
    const bool eligible = current && message == "R" && !m_stopping && !m_preview->stopping() &&
            !m_requested && m_restarts.size() < 3;
    if (eligible && socket->write("A", 1) == 1 && socket->flush()) {
        m_requested = true;
        m_restarts.append(now);
        m_preview->expectRestart();
        QPointer<QProcess> process = m_process;
        qInfo("Aurora Store restart accepted for worker pid=%lld.",
              static_cast<long long>(process->processId()));
        QTimer::singleShot(500, Qt::PreciseTimer, this, [this, process]() {
            if (!process || process != m_process || !m_requested || m_stopping ||
                    m_preview->stopping() || process->state() == QProcess::NotRunning) return;
            qWarning("Aurora Store restart: worker did not exit in time; terminating pid=%lld.",
                     static_cast<long long>(process->processId()));
            m_restartKillRequested = true;
            process->kill();
        });
    } else {
        qWarning("Aurora Store restart request rejected.");
        if (!eligible) {
            socket->write("N", 1);
            socket->flush();
        }
    }
    socket->disconnectFromServer();
}

void StorePreviewSupervisor::stop()
{
    if (m_stopping) return;
    m_stopping = true;
    qInfo("Aurora Store preview stop requested; pending restart=%d.", m_requested);
    m_preview->stop();
}
