#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_PICKER_HOST_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_PICKER_HOST_H

#include <QObject>
#include <QMutex>
#include <QStringList>

#include "../core/include/megler.h"

class PickerCopyThread;
class QTemporaryDir;

class AuroraPickerHost : public QObject
{
    Q_OBJECT
public:
    explicit AuroraPickerHost(QObject *parent = nullptr);
    ~AuroraPickerHost() override;

    // Keep this object alive until the core can no longer invoke its callbacks.
    megler_system_host host();
    void setDeniedPermissions(const QStringList &denied) { m_deniedPermissions = denied; }
    void setPickerCapabilities(bool multiple, bool photo);
    // UI thread only; call before stopping the core, while it can receive the answer.
    void stop();

    Q_INVOKABLE void setUiReady(bool ready);
    Q_INVOKABLE void selected(const QString &token, const QString &path);
    Q_INVOKABLE void selectedMany(const QString &token, const QStringList &paths);
    Q_INVOKABLE void validatePhoto(const QString &token, const QString &path);
    Q_INVOKABLE void confirmPhoto(const QString &token);
    Q_INVOKABLE void canceled(const QString &token);
    Q_INVOKABLE void showFailed(const QString &token);

signals:
    void showPicker(const QString &token, const QStringList &filters, bool multiple);
    void showPhoto(const QString &token, const QString &path);
    void closePhoto(const QString &token);
    void photoValidated(const QString &token, bool valid);

private slots:
    void dispatch(const QString &token);
    void copyFinished();

private:
    static bool MEGLER_CB pick(void *user, const megler_pick_request *request);
    static int MEGLER_CB permission(void *user, const char *group, bool ask);
    void finish(int status, const QList<QByteArray> &names = {});

    QMutex m_mutex;
    QStringList m_deniedPermissions;
    bool m_ready = false;
    bool m_supportsMultiple = true;
    bool m_supportsPhoto = true;
    bool m_active = false;
    quint64 m_serial = 0;
    unsigned m_id = 0;
    int m_kind = MEGLER_PICK_FILES;
    QString m_folder;
    QStringList m_types;
    bool m_multiple = false;
    int m_max = 0;
    PickerCopyThread *m_copy = nullptr;
    QTemporaryDir *m_photoTemp = nullptr;
    QString m_photoPath;
    bool m_photoSaved = false;
};

#endif
