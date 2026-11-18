#ifndef MEGLER_AURORA_INSTALL_REQUEST_COMMIT_H
#define MEGLER_AURORA_INSTALL_REQUEST_COMMIT_H

#include <QThread>
#include <QString>
#include <atomic>

// Stages an immutable snapshot into an unnamed file off the UI thread.
// Commit is called on the owner thread only after confirmation and only while
// the sender is live. A filesystem without O_TMPFILE fails closed.
class InstallRequestCommit : public QThread
{
public:
    InstallRequestCommit(const QString &source, const QString &root, const QString &package,
                         bool allowUpdate = false);
    ~InstallRequestCommit() override;
    void cancel();
    bool commit(QString *error);
    QString error() const { return m_error; }

protected:
    void run() override;

private:
    QString m_source, m_root, m_package, m_error;
    std::atomic<bool> m_canceled;
    int m_file;
    int m_rootFd;
    bool m_allowUpdate;
};

#endif
