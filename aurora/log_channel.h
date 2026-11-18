#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_LOG_CHANNEL_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_LOG_CHANNEL_H

#include <QObject>
#include <QString>
#include <QTemporaryDir>

#include "../core/include/megler.h"

class QSocketNotifier;
class QTimer;

class LogChannel : public QObject
{
    Q_OBJECT
public:
    explicit LogChannel(QObject *parent = nullptr);
    ~LogChannel() override;
    QString path() const { return m_path; }
    QString text() const { return m_text; }
    bool available() const { return m_socket >= 0; }
    static int openSender(const QString &path);
    static void MEGLER_CB sendLine(int level, const char *line, void *user);

signals:
    void changed();

private:
    void readMessages();
    QTemporaryDir m_directory;
    QString m_path;
    QString m_text;
    int m_socket = -1;
    QSocketNotifier *m_notifier = nullptr;
    QTimer *m_update = nullptr;
};

#endif
