#include "standalone_start.h"

#include <RuntimeManager/IntentsInvoker>

#include <QJsonObject>
#include <QRegularExpression>
#include <QTimer>

StandaloneStart::StandaloneStart(QObject *parent)
    : QObject(parent), m_invoker(new RuntimeManager::IntentsInvoker(this)),
      m_timeout(new QTimer(this))
{
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this, [this]() {
        finish(false, tr("The app launch request timed out."));
    });
    connect(m_invoker, &RuntimeManager::IntentsInvoker::replyReceived, this,
            [this](const QJsonObject &, const RuntimeManager::Error &error) {
        QString message = error ? error.message() : QString();
        if (error && message.isEmpty())
            message = tr("The app launch request failed.");
        finish(!error, message);
    });
}

bool StandaloneStart::start(const QString &appId, QString *error)
{
    static const QRegularExpression validId(
        QStringLiteral("\\Aru\\.androidcompatibilitylayer\\.Android[0-9a-f]{24}\\z"));
    if (m_started || !validId.match(appId).hasMatch()) {
        if (error) *error = tr("Invalid or already submitted app launch request.");
        return false;
    }
    if (error) error->clear();
    m_started = true;
    QJsonObject hints;
    hints.insert(QStringLiteral("preferredHandler"), appId);
    hints.insert(QStringLiteral("onlyPreferred"), true);
    m_timeout->start(30000);
    m_invoker->invoke(QStringLiteral("Start"), hints, QJsonObject());
    return true;
}

void StandaloneStart::finish(bool accepted, const QString &error)
{
    if (!m_started || m_finished) return;
    m_finished = true;
    m_timeout->stop();
    emit finished(accepted, error);
}
