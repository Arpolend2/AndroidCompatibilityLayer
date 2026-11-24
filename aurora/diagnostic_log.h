#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_DIAGNOSTIC_LOG_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_DIAGNOSTIC_LOG_H

#include <QObject>
#include <QString>
#include <QtGlobal>
#include <functional>
#include <memory>

class QProcess;
class QTimer;

class DiagnosticLog : public QObject
{
    Q_OBJECT
public:
    explicit DiagnosticLog(QObject *parent = nullptr);
    ~DiagnosticLog() override;

    bool start(QString *error, bool persistStreams = false);
    void startNotifications();
    static void installChildQtMessageHandler();
    void watch(QProcess *process, const QString &source,
               const std::function<void(bool, const QByteArray &)> &observer = {});
    QString text() const;
    bool available() const;
    Q_INVOKABLE QString standaloneDiagnosticFileError() const;
    Q_INVOKABLE QString overlayText() const;

signals:
    void changed();

private:
    struct State;
    static std::shared_ptr<State> s_messageState;
    static QtMessageHandler s_previousHandler;
    static void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message);
    static void childMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message);
    std::shared_ptr<State> m_state;
    QTimer *m_timer;
    void append(const QString &source, const QByteArray &bytes, int output);
    void finish(const QString &source);
};

#endif
