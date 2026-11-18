#ifndef MEGLER_AURORA_EXTERNAL_APK_RECEIVER_H
#define MEGLER_AURORA_EXTERNAL_APK_RECEIVER_H

#include <QObject>
#include <QStringList>
#include <QtDBus/qdbusmacros.h>

class ExternalApkReceiver : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "ru.megler.MeglerProbe")
public:
    explicit ExternalApkReceiver(QObject *parent = nullptr);
    ~ExternalApkReceiver() override;
    bool start(QString *error);
    void submit(const QStringList &arguments);

public slots:
    Q_SCRIPTABLE Q_NOREPLY void openFile(const QStringList &arguments);

signals:
    void requested(const QString &path);
    void rejected(const QString &message);

private:
    bool m_legacyRegistered;
    bool m_runtimeStarted;
};

#endif
