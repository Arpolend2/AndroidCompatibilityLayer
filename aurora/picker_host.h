#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_PICKER_HOST_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_PICKER_HOST_H

#include <QObject>
#include <QMutex>
#include <QStringList>

#include "../core/include/megler.h"

class PickerCopyThread;

class AuroraPickerHost : public QObject
{
    Q_OBJECT
public:
    explicit AuroraPickerHost(QObject *parent = nullptr);
    ~AuroraPickerHost() override;

    // Keep this object alive until the core can no longer invoke its callbacks.
    megler_system_host host();
    void setDeniedPermissions(const QStringList &denied) { m_deniedPermissions = denied; }
    // UI thread only; call before stopping the core, while it can receive the answer.
    void stop();

    Q_INVOKABLE void setUiReady(bool ready);
    Q_INVOKABLE void selected(const QString &token, const QString &path);
    Q_INVOKABLE void canceled(const QString &token);
    Q_INVOKABLE void showFailed(const QString &token);

signals:
    void showPicker(const QString &token, const QStringList &filters);

private slots:
    void dispatch(const QString &token);
    void copyFinished();

private:
    static bool MEGLER_CB pick(void *user, const megler_pick_request *request);
    static int MEGLER_CB permission(void *user, const char *group, bool ask);
    void finish(int status, const QByteArray &name = QByteArray());

    QMutex m_mutex;
    QStringList m_deniedPermissions;
    bool m_ready = false;
    bool m_active = false;
    quint64 m_serial = 0;
    unsigned m_id = 0;
    QString m_folder;
    QStringList m_types;
    PickerCopyThread *m_copy = nullptr;
};

#endif
