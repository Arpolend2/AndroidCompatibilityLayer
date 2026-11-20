#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_INSTALL_REQUEST_COORDINATOR_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_INSTALL_REQUEST_COORDINATOR_H

#include "install_request_channel.h"
#include "install_request_prepare.h"
#include "install_request_commit.h"

class QProcess;

class InstallRequestCoordinator : public QObject
{
    Q_OBJECT
public:
    InstallRequestCoordinator(QProcess *process, const QString &senderRoot, const QString &storeRoot, QObject *parent = nullptr);
    ~InstallRequestCoordinator() override;
    bool listen(QString *error) { return m_server.listen(error); }
    QString endpoint() const { return m_server.endpoint(); }
    QString resultsDir() const { return m_server.resultsDir(); }
    void decision(quint64 id, bool accepted);
    void unavailable(quint64 id);

signals:
    void installed(const QString &package);
    void confirmationRequested(quint64 id, const QString &label, const QString &packageName, quint32 version);
    void confirmationCanceled(quint64 id);

private:
    void request(quint64 id, const QString &path, const QString &name);
    void ready(quint64 id);
    void canceled(quint64 id);
    void staged();

    QProcess *m_process;
    QString m_senderRoot, m_storeRoot;
    InstallRequestServer m_server;
    InstallRequestPrepare m_prepare;
    InstallRequestCommit *m_commit;
    quint64 m_active;
    quint64 m_commitId;
    bool m_prompting;
};

#endif
