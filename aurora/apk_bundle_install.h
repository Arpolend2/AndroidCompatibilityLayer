#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_APK_BUNDLE_INSTALL_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_APK_BUNDLE_INSTALL_H

#include <QObject>
#include <QProcess>
#include <QString>

class QTemporaryDir;
class QThread;

class ApkBundleInstall : public QObject
{
    Q_OBJECT

public:
    ApkBundleInstall(const QString &storeRoot, const QString &androidRoot, int densityDpi,
                     const QString &languages, QObject *parent = nullptr);
    ~ApkBundleInstall() override;

    bool start(const QString &path, QString *error, bool storeHandoff = false);
    bool busy() const { return m_temporaryDirectory != nullptr; }
    void setLanguages(const QString &languages) { m_languages = languages; }
    void decision(bool accepted);
    void unavailable();

signals:
    void confirmationRequested(const QString &label, const QString &package, quint32 version);
    void finished(bool installed, const QString &message, const QString &package, const QByteArray &fingerprint);

private:
    void startImporter(const QString &source, const QString &suffix, bool storeHandoff);
    void processFinished(int exitCode, QProcess::ExitStatus status);
    void finish(bool installed, const QString &message);

    QString m_storeRoot;
    QString m_androidRoot;
    int m_densityDpi;
    QString m_languages;
    QTemporaryDir *m_temporaryDirectory;
    QProcess *m_process;
    QThread *m_snapshotWorker = nullptr;
    QString m_package;
    bool m_prompting = false;
    bool m_storeHandoff = false;
    QByteArray m_fingerprint;
};

#endif
