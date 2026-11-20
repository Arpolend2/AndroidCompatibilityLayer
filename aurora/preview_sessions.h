#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_PREVIEW_SESSIONS_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_PREVIEW_SESSIONS_H

#include <QObject>
#include <QString>
#include <QList>
#include <QPointer>

class PreviewSession;
class DiagnosticLog;
class QProcess;

class EmbeddedEndpoint
{
public:
    virtual ~EmbeddedEndpoint() {}
    virtual bool listen(QProcess *process, QString *error) = 0;
    virtual QString endpoint() const = 0;
    virtual bool stopping() const = 0;
};

// The owner must remain alive until all children have exited.
class PreviewSessions : public QObject
{
    Q_OBJECT
public:
    explicit PreviewSessions(QObject *parent = nullptr);
    ~PreviewSessions() override;

    void setDiagnosticLog(DiagnosticLog *log);
    bool start(const QString &key, const QString &androidRoot, const QString &data,
               const QString &apk, const QString &storeRoot, QString *error,
               EmbeddedEndpoint *embedded = nullptr);
    bool activate(const QString &key, int timeoutMs, QString *error);
    bool running(const QString &key) const;
    bool kill(const QString &key);
    int count() const;
    void decision(const QString &token, bool accepted);
    void unavailable(const QString &token);

signals:
    void runningChanged();
    void started(const QString &key);
    void finished(const QString &instance, const QString &key, int exitCode, bool crashed, const QString &error);
    void activationFinished(const QString &instance, const QString &key, bool activated, const QString &error);
    void installed(const QString &package);
    void logChanged(const QString &instance, const QString &key, const QString &text);
    void logUnavailable(const QString &key);
    void confirmationRequested(const QString &token, const QString &label,
                               const QString &package, quint32 version);
    void confirmationCanceled(const QString &token);

private:
    void finish(PreviewSession *session, int code, bool crashed, const QString &error);
    PreviewSession *pending(const QString &token) const;
    QList<PreviewSession *> m_sessions;
    QPointer<DiagnosticLog> m_diagnosticLog;
};

#endif
