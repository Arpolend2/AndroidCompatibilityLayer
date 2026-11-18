#include "install_request_channel.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QMutexLocker>

#include <cstring>
#include <sys/socket.h>
#include <sys/types.h>

namespace {
const int maxFrame = 8192;
const int maxPath = 4096;
const int maxName = 255;
const int maxMessage = 1024;

bool validOutcome(const QString &value)
{
    return value == QLatin1String("installed") || value == QLatin1String("declined") ||
           value == QLatin1String("incompatible") || value == QLatin1String("failed");
}

QByteArray frame(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

bool takeFrame(QByteArray *input, QLocalSocket *socket, QJsonObject *object)
{
    *input += socket->readAll();
    const int end = input->indexOf('\n');
    if (input->size() > maxFrame || end < 0 || end != input->size() - 1) return false;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(input->left(end), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
    *object = document.object();
    input->clear();
    return true;
}

bool completeFrameAvailable(const QByteArray &input, QLocalSocket *socket)
{
    return input.contains('\n') || socket->bytesAvailable() > maxFrame ||
           socket->peek(socket->bytesAvailable()).contains('\n');
}
}

InstallRequestServer::InstallRequestServer(QProcess *process, QObject *parent)
    : QObject(parent), m_process(process), m_server(new QLocalServer(this)),
      m_socket(nullptr), m_serial(0), m_active(0), m_confirmed(false)
{
    connect(m_server, &QLocalServer::newConnection, this, &InstallRequestServer::acceptPending);
    connect(m_process, &QProcess::started, this, &InstallRequestServer::acceptPending);
    connect(m_process, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, [this](int, QProcess::ExitStatus) { closeConnection(); });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) closeConnection();
    });
}

InstallRequestServer::~InstallRequestServer()
{
    closeConnection();
    m_server->close();
}

bool InstallRequestServer::listen(QString *error)
{
    if (!m_directory.isValid()) {
        if (error) *error = tr("Cannot create the installation channel directory.");
        return false;
    }
    if (!QDir(m_directory.path()).mkdir(QStringLiteral("results"))) {
        if (error) *error = tr("Cannot create the installation result directory.");
        return false;
    }
    if (!m_server->listen(m_directory.path() + QStringLiteral("/channel"))) {
        if (error) *error = m_server->errorString();
        return false;
    }
    return true;
}

QString InstallRequestServer::resultsDir() const
{
    return m_directory.isValid() ? m_directory.path() + QStringLiteral("/results") : QString();
}

QString InstallRequestServer::endpoint() const
{
    return m_server->isListening() ? m_server->fullServerName() : QString();
}

void InstallRequestServer::acceptPending()
{
    if (m_process->processId() <= 0) return;
    while (m_server->hasPendingConnections()) {
        QLocalSocket *socket = m_server->nextPendingConnection();
        struct ucred credentials;
        socklen_t size = sizeof(credentials);
        if (m_socket || getsockopt(static_cast<int>(socket->socketDescriptor()), SOL_SOCKET, SO_PEERCRED,
                                   &credentials, &size) != 0 ||
            size != sizeof(credentials) || credentials.pid != m_process->processId()) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        m_socket = socket;
        m_socket->setReadBufferSize(maxFrame + 1);
        connect(m_socket, &QLocalSocket::readyRead, this, &InstallRequestServer::readAvailable);
        connect(m_socket, &QLocalSocket::disconnected, this, &InstallRequestServer::disconnected);
        if (m_socket->bytesAvailable()) readAvailable();
    }
}

void InstallRequestServer::readAvailable()
{
    if (!m_socket) return;
    if (!completeFrameAvailable(m_input, m_socket)) {
        m_input += m_socket->readAll();
        if (m_input.size() > maxFrame) closeConnection();
        return;
    }
    QJsonObject request;
    if (m_active || !takeFrame(&m_input, m_socket, &request) ||
        request.size() != 2 || !request.value(QStringLiteral("path")).isString() ||
        !request.value(QStringLiteral("name")).isString()) {
        closeConnection();
        return;
    }
    const QString path = request.value(QStringLiteral("path")).toString();
    const QString name = request.value(QStringLiteral("name")).toString();
    if (path.isEmpty() || path.toUtf8().size() > maxPath || path.contains(QChar(0)) ||
        name.isEmpty() || name.toUtf8().size() > maxName || name.contains(QChar(0))) {
        closeConnection();
        return;
    }
    if (++m_serial == 0) ++m_serial;
    m_active = m_serial;
    emit requested(m_active, path, name);
}

bool InstallRequestServer::approve(quint64 id)
{
    if (!m_socket || m_process->state() != QProcess::Running || !id || id != m_active || m_confirmed) return false;
    m_confirmed = true;
    return true;
}

bool InstallRequestServer::finish(quint64 id, const QString &outcome, const QString &message,
                                  const QString &package, const QString &label, quint32 version)
{
    if (!m_socket || m_process->state() != QProcess::Running || !id || id != m_active ||
        (outcome == QLatin1String("installed") && !m_confirmed) ||
        (outcome == QLatin1String("installed") && (package.isEmpty() || package.size() > 255 || label.toUtf8().size() > 1024)) ||
        !validOutcome(outcome) || message.toUtf8().size() > maxMessage) return false;
    QJsonObject response;
    response.insert(QStringLiteral("outcome"), outcome);
    response.insert(QStringLiteral("message"), message);
    if (outcome == QLatin1String("installed")) {
        response.insert(QStringLiteral("package"), package);
        response.insert(QStringLiteral("label"), label);
        response.insert(QStringLiteral("version"), static_cast<double>(version));
    }
    const QByteArray output = frame(response);
    if (m_socket->write(output) != output.size()) {
        closeConnection();
        return false;
    }
    m_active = 0;
    m_confirmed = false;
    return true;
}

void InstallRequestServer::disconnected()
{
    closeConnection();
}

void InstallRequestServer::closeConnection()
{
    QLocalSocket *socket = m_socket;
    m_socket = nullptr;
    m_input.clear();
    const quint64 active = m_active;
    m_active = 0;
    m_confirmed = false;
    if (socket) {
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }
    if (active) emit canceled(active);
}

InstallRequestClient::InstallRequestClient(const QString &endpoint, QObject *parent)
    : QObject(parent), m_socket(new QLocalSocket(this)), m_connected(false), m_busy(false)
{
    m_socket->setReadBufferSize(maxFrame + 1);
    connect(m_socket, &QLocalSocket::connected, this, [this]() {
        QMutexLocker lock(&m_mutex);
        m_connected = true;
    });
    connect(m_socket, &QLocalSocket::readyRead, this, &InstallRequestClient::readAvailable);
    connect(m_socket, &QLocalSocket::disconnected, this, &InstallRequestClient::disconnected);
    connect(m_socket, static_cast<void (QLocalSocket::*)(QLocalSocket::LocalSocketError)>(&QLocalSocket::error),
            this, [this](QLocalSocket::LocalSocketError) { disconnected(); });
    connect(this, &InstallRequestClient::sendRequested, this, &InstallRequestClient::sendQueued, Qt::QueuedConnection);
    m_socket->connectToServer(endpoint);
}

InstallRequestClient::~InstallRequestClient()
{
    m_socket->disconnect(this);
    m_socket->abort();
}

bool InstallRequestClient::submit(const char *path, const char *name)
{
    if (!path || !name) return false;
    const size_t pathSize = strnlen(path, maxPath + 1);
    const size_t nameSize = strnlen(name, maxName + 1);
    if (!pathSize || pathSize > maxPath || !nameSize || nameSize > maxName) return false;
    const QByteArray pathBytes(path, static_cast<int>(pathSize));
    const QByteArray nameBytes(name, static_cast<int>(nameSize));
    const QString pathText = QString::fromUtf8(pathBytes);
    const QString nameText = QString::fromUtf8(nameBytes);
    if (pathText.toUtf8() != pathBytes || nameText.toUtf8() != nameBytes) return false;
    QJsonObject request;
    request.insert(QStringLiteral("path"), pathText);
    request.insert(QStringLiteral("name"), nameText);
    const QByteArray output = frame(request);
    if (output.size() > maxFrame) return false;
    QMutexLocker lock(&m_mutex);
    if (!m_connected || m_busy) return false;
    m_busy = true;
    m_pending = output;
    emit sendRequested();
    return true;
}

void InstallRequestClient::sendQueued()
{
    QByteArray output;
    {
        QMutexLocker lock(&m_mutex);
        output.swap(m_pending);
        if (!m_connected || !m_busy) return;
    }
    if (output.isEmpty() || m_socket->write(output) != output.size()) {
        m_socket->abort();
        disconnected();
    }
}

void InstallRequestClient::readAvailable()
{
    if (!completeFrameAvailable(m_input, m_socket)) {
        m_input += m_socket->readAll();
        if (m_input.size() > maxFrame) {
            m_socket->abort();
            disconnected();
        }
        return;
    }
    QJsonObject response;
    if (!takeFrame(&m_input, m_socket, &response)) {
        m_socket->abort();
        disconnected();
        return;
    }
    const bool installed = response.value(QStringLiteral("outcome")).toString() == QLatin1String("installed");
    const double version = response.value(QStringLiteral("version")).toDouble(-1);
    if (response.size() != (installed ? 5 : 2) ||
        !response.value(QStringLiteral("outcome")).isString() ||
        !response.value(QStringLiteral("message")).isString() ||
        !validOutcome(response.value(QStringLiteral("outcome")).toString()) ||
        response.value(QStringLiteral("message")).toString().toUtf8().size() > maxMessage ||
        (installed && (!response.value(QStringLiteral("package")).isString() ||
                       !response.value(QStringLiteral("label")).isString() ||
                       !response.value(QStringLiteral("version")).isDouble() ||
                       version < 0 || version > 4294967295.0 || static_cast<quint32>(version) != version))) {
        m_socket->abort();
        disconnected();
        return;
    }
    {
        QMutexLocker lock(&m_mutex);
        if (!m_busy || !m_pending.isEmpty()) {
            lock.unlock();
            m_socket->abort();
            disconnected();
            return;
        }
        m_busy = false;
    }
    emit completed(response.value(QStringLiteral("outcome")).toString(),
                   response.value(QStringLiteral("message")).toString(),
                   response.value(QStringLiteral("package")).toString(),
                   response.value(QStringLiteral("label")).toString(),
                   installed ? static_cast<quint32>(version) : 0);
}

void InstallRequestClient::disconnected()
{
    bool pending;
    {
        QMutexLocker lock(&m_mutex);
        m_connected = false;
        pending = m_busy;
        m_busy = false;
        m_pending.clear();
    }
    m_input.clear();
    if (pending) emit completed(QStringLiteral("failed"), tr("Installation channel disconnected."),
                                QString(), QString(), 0);
}
