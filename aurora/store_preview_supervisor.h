#ifndef ANDROID_COMPATIBILITY_LAYER_STORE_PREVIEW_SUPERVISOR_H
#define ANDROID_COMPATIBILITY_LAYER_STORE_PREVIEW_SUPERVISOR_H

#include <QObject>
#include <QElapsedTimer>
#include <QLocalServer>
#include <QPointer>
#include <QProcess>
#include <QStringList>
#include <QTemporaryDir>

class EmbeddedPreview;

class StorePreviewSupervisor : public QObject
{
    Q_OBJECT
public:
    StorePreviewSupervisor(EmbeddedPreview *preview, const QString &program,
                           const QStringList &arguments, QObject *parent = nullptr);
    bool start(QString *error);
    void stop();
signals:
    void finished(int code, bool crashed);
private:
    bool launch(QString *error);
    void acceptPending();
    void request(class QLocalSocket *socket);
    EmbeddedPreview *m_preview;
    QString m_program;
    QStringList m_arguments;
    QTemporaryDir m_directory;
    QLocalServer m_server;
    QPointer<QProcess> m_process;
    QElapsedTimer m_clock;
    QList<qint64> m_restarts;
    bool m_requested = false;
    bool m_restartKillRequested = false;
    bool m_stopTeardownWatchdogArmed = false;
    bool m_stopping = false;
};

#endif
