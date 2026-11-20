#ifndef ANDROID_COMPATIBILITY_LAYER_STANDALONE_START_H
#define ANDROID_COMPATIBILITY_LAYER_STANDALONE_START_H

#include <QObject>
#include <QString>

class QTimer;
namespace RuntimeManager { class IntentsInvoker; }

class StandaloneStart : public QObject
{
    Q_OBJECT
public:
    explicit StandaloneStart(QObject *parent = nullptr);
    bool start(const QString &appId, QString *error);

signals:
    void finished(bool accepted, const QString &error);

private:
    void finish(bool accepted, const QString &error);
    RuntimeManager::IntentsInvoker *m_invoker;
    QTimer *m_timeout;
    bool m_started = false;
    bool m_finished = false;
};

#endif
