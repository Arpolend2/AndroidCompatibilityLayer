#include "preview_activation_channel.h"

#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QTimer>
#include <QWindow>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>

namespace {
const int maxFrame = 128;

qint64 monotonicMs()
{
    timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) return -1;
    return qint64(time.tv_sec) * 1000 + time.tv_nsec / 1000000;
}

bool parseNumber(const QByteArray &text, quint64 *value)
{
    if (text.isEmpty() || text.size() > 20 || text.startsWith('0')) return false;
    for (char c : text)
        if (c < '0' || c > '9') return false;
    bool ok = false;
    *value = text.toULongLong(&ok);
    return ok && *value != 0;
}

}

PreviewActivationServer::PreviewActivationServer(QProcess *process, QObject *parent)
    : QObject(parent), m_process(process), m_server(new QLocalServer(this)), m_timer(new QTimer(this))
{
    m_timer->setSingleShot(true);
    m_timer->setTimerType(Qt::PreciseTimer);
    connect(m_timer, &QTimer::timeout, this, [this]() {
        finish(false, tr("The preview window did not become active in time."));
    });
    connect(m_server, &QLocalServer::newConnection, this, &PreviewActivationServer::acceptPending);
    connect(process, &QProcess::started, this, &PreviewActivationServer::acceptPending);
    connect(process, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, [this](int, QProcess::ExitStatus) {
        closeConnection(tr("The preview process exited."));
    });
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            closeConnection(tr("The preview process could not start."));
    });
}

PreviewActivationServer::~PreviewActivationServer()
{
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
    }
}

bool PreviewActivationServer::listen(QString *error)
{
    if (error) error->clear();
    if (!m_directory.isValid()) {
        if (error) *error = tr("Cannot create the preview activation directory.");
        return false;
    }
    if (!m_server->listen(m_directory.path() + QStringLiteral("/channel"))) {
        if (error) *error = m_server->errorString();
        return false;
    }
    return true;
}

QString PreviewActivationServer::endpoint() const
{
    return m_server->isListening() ? m_server->fullServerName() : QString();
}

bool PreviewActivationServer::request(int timeoutMs, QString *error)
{
    if (error) error->clear();
    if (m_pending || timeoutMs <= 0 || !m_server->isListening() || m_disconnected ||
        m_process->state() == QProcess::NotRunning) {
        if (error) *error = tr("The preview activation channel is busy or unavailable.");
        return false;
    }
    const qint64 now = monotonicMs();
    if (now < 0) {
        if (error) *error = tr("Cannot read the monotonic clock.");
        return false;
    }
    if (++m_serial == 0) ++m_serial;
    m_pending = m_serial;
    m_deadline = now + timeoutMs;
    m_sent = false;
    m_timer->start(timeoutMs);
    sendPending();
    return true;
}

void PreviewActivationServer::acceptPending()
{
    if (m_process->processId() <= 0) return;
    while (m_server->hasPendingConnections()) {
        QLocalSocket *socket = m_server->nextPendingConnection();
        struct ucred credentials;
        socklen_t size = sizeof(credentials);
        if (m_socket || m_disconnected ||
            getsockopt(static_cast<int>(socket->socketDescriptor()), SOL_SOCKET, SO_PEERCRED,
                       &credentials, &size) != 0 ||
            size != sizeof(credentials) || credentials.pid != m_process->processId()) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        m_socket = socket;
        m_socket->setReadBufferSize(maxFrame + 1);
        connect(socket, &QLocalSocket::readyRead, this, &PreviewActivationServer::readAvailable);
        connect(socket, &QLocalSocket::disconnected, this, [this]() {
            closeConnection(tr("The preview activation channel disconnected."));
        });
        if (socket->bytesAvailable()) readAvailable();
        sendPending();
    }
}

void PreviewActivationServer::sendPending()
{
    if (!m_socket || !m_pending || m_sent) return;
    const qint64 now = monotonicMs();
    if (now < 0 || now > m_deadline) {
        finish(false, now < 0 ? tr("Cannot read the monotonic clock.") :
               tr("The preview window did not become active in time."));
        return;
    }
    const QByteArray output = QByteArray("A ") + QByteArray::number(m_pending) + ' ' +
                              QByteArray::number(m_deadline) + '\n';
    m_sent = true;
    if (m_socket->write(output) != output.size())
        closeConnection(tr("Could not send the preview activation request."));
}

void PreviewActivationServer::readAvailable()
{
    if (!m_socket) return;
    m_input += m_socket->readAll();
    int end;
    while ((end = m_input.indexOf('\n')) >= 0) {
        if (end > maxFrame) {
            closeConnection(tr("Invalid preview activation response."));
            return;
        }
        const QByteArray line = m_input.left(end);
        m_input.remove(0, end + 1);
        quint64 id;
        if (line.size() < 3 || (line[0] != 'O' && line[0] != 'E') || line[1] != ' ' ||
            !parseNumber(line.mid(2), &id) || id > m_serial) {
            closeConnection(tr("Invalid preview activation response."));
            return;
        }
        if (id == m_pending) {
            const qint64 now = monotonicMs();
            const bool activated = line[0] == 'O' && now >= 0 && now <= m_deadline;
            finish(activated, activated ? QString() : (line[0] == 'E' ?
                   tr("The preview window could not be activated.") :
                   tr("The preview window did not become active in time.")));
        }
    }
    if (m_input.size() > maxFrame)
        closeConnection(tr("Invalid preview activation response."));
}

void PreviewActivationServer::finish(bool activated, const QString &error)
{
    if (!m_pending) return;
    m_pending = 0;
    m_sent = false;
    m_timer->stop();
    emit completed(activated, error);
}

void PreviewActivationServer::closeConnection(const QString &error)
{
    m_disconnected = true;
    QLocalSocket *socket = m_socket;
    m_socket = nullptr;
    m_input.clear();
    if (socket) {
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }
    finish(false, error);
}

PreviewActivationClient::PreviewActivationClient(const QString &endpoint, QWindow *window, QObject *parent)
    : QObject(parent), m_socket(new QLocalSocket(this)), m_endpoint(endpoint), m_window(window)
{
    m_socket->setReadBufferSize(maxFrame + 1);
    connect(m_socket, &QLocalSocket::readyRead, this, &PreviewActivationClient::readAvailable);
    connect(m_socket, &QLocalSocket::disconnected, this, [this]() {
        fail(tr("The preview activation channel disconnected."));
    });
    connect(m_socket, static_cast<void (QLocalSocket::*)(QLocalSocket::LocalSocketError)>(&QLocalSocket::error),
            this, [this](QLocalSocket::LocalSocketError) { fail(m_socket->errorString()); });
    if (window) connect(window, &QWindow::activeChanged, this, &PreviewActivationClient::activeChanged);
}

bool PreviewActivationClient::start(QString *error)
{
    if (error) error->clear();
    if (m_endpoint.isEmpty() || m_failed || m_socket->state() != QLocalSocket::UnconnectedState) {
        if (error) *error = tr("The preview activation endpoint is invalid or already connected.");
        return false;
    }
    m_socket->connectToServer(m_endpoint);
    if (m_failed) {
        if (error) *error = m_socket->errorString();
        return false;
    }
    return true;
}

void PreviewActivationClient::readAvailable()
{
    m_input += m_socket->readAll();
    int end;
    while ((end = m_input.indexOf('\n')) >= 0) {
        if (end > maxFrame) {
            fail(tr("Invalid preview activation request."));
            return;
        }
        const QByteArray line = m_input.left(end);
        m_input.remove(0, end + 1);
        const QList<QByteArray> parts = line.split(' ');
        quint64 id, deadline;
        if (parts.size() != 3 || parts[0] != "A" || !parseNumber(parts[1], &id) ||
            !parseNumber(parts[2], &deadline) || deadline > quint64(Q_INT64_C(9223372036854775807))) {
            fail(tr("Invalid preview activation request."));
            return;
        }
        m_pending = id;
        m_deadline = qint64(deadline);
        const qint64 now = monotonicMs();
        if (!m_window || now < 0 || now > m_deadline) {
            respond(id, false);
        } else if (m_window->isActive()) {
            respond(id, true);
        } else {
            m_window->raise();
            m_window->requestActivate();
            activeChanged();
        }
    }
    if (m_input.size() > maxFrame)
        fail(tr("Invalid preview activation request."));
}

void PreviewActivationClient::activeChanged()
{
    if (!m_pending || !m_window || !m_window->isActive()) return;
    const qint64 now = monotonicMs();
    respond(m_pending, now >= 0 && now <= m_deadline);
}

void PreviewActivationClient::respond(quint64 id, bool activated)
{
    if (m_pending == id) m_pending = 0;
    const QByteArray output = QByteArray(activated ? "O " : "E ") + QByteArray::number(id) + '\n';
    if (m_socket->write(output) != output.size())
        fail(tr("Could not send the preview activation response."));
}

void PreviewActivationClient::fail(const QString &error)
{
    if (m_failed) return;
    m_failed = true;
    m_pending = 0;
    m_socket->abort();
    emit channelError(error);
}
