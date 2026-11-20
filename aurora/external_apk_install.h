#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_EXTERNAL_APK_INSTALL_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_EXTERNAL_APK_INSTALL_H

#include "install_request_prepare.h"
#include <QObject>

class InstallRequestCommit;

class ExternalApkInstall : public QObject
{
    Q_OBJECT
public:
    explicit ExternalApkInstall(const QString &storeRoot, QObject *parent = nullptr);
    ~ExternalApkInstall() override;

    bool start(const QString &path, QString *error, const QString &expectedPackage = QString(),
               bool allowUpdate = false, bool storeHandoff = false);
    bool busy() const;
    bool isUpdate() const { return m_updating; }
    void decision(bool accepted);
    void unavailable();

signals:
    void confirmationRequested(const QString &label, const QString &packageName, quint32 version);
    void finished(bool installed, const QString &message, const QString &package, const QByteArray &fingerprint);

private:
    void ready(quint64 id);
    void staged();
    void finish(bool installed, const QString &message, const QString &package = QString(),
                const QByteArray &fingerprint = QByteArray());

    QString m_storeRoot;
    QString m_expectedPackage;
    InstallRequestPrepare m_prepare;
    InstallRequestCommit *m_commit;
    bool m_active;
    bool m_prompting;
    bool m_allowUpdate;
    bool m_updating;
};

#endif
