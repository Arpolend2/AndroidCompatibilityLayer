#include "picker_host.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QMimeDatabase>
#include <QMimeType>
#include <QMutexLocker>
#include <QThread>

namespace {
bool matchesType(const QString &name, const QStringList &types)
{
    if (types.isEmpty() || types.contains(QStringLiteral("*/*"))) return true;
    QMimeDatabase db;
    const QMimeType mime = db.mimeTypeForFile(name, QMimeDatabase::MatchExtension);
    for (const QString &type : types) {
        if (type.endsWith(QStringLiteral("/*"))) {
            if (mime.name().startsWith(type.left(type.size() - 1))) return true;
        } else if (mime.inherits(type) || mime.name() == type) {
            return true;
        }
    }
    return false;
}

bool filtersFor(const QStringList &types, QStringList *filters)
{
    if (types.isEmpty() || types.contains(QStringLiteral("*/*"))) {
        *filters = QStringList(QStringLiteral("*"));
        return true;
    }
    QMimeDatabase db;
    for (const QString &type : types) {
        bool known = false;
        const QList<QMimeType> all = type.endsWith(QStringLiteral("/*"))
                ? db.allMimeTypes() : QList<QMimeType>() << db.mimeTypeForName(type);
        for (const QMimeType &mime : all) {
            if (!mime.isValid() || mime.isDefault()) continue;
            if (type.endsWith(QStringLiteral("/*")) &&
                !mime.name().startsWith(type.left(type.size() - 1))) continue;
            const QStringList patterns = mime.globPatterns();
            if (!patterns.isEmpty()) known = true;
            for (const QString &pattern : patterns) {
                if (!filters->contains(pattern)) filters->append(pattern);
            }
        }
        if (!known) return false;
    }
    return !filters->isEmpty();
}
}

class PickerCopyThread : public QThread
{
public:
    PickerCopyThread(const QString &path, const QString &folder, const QStringList &types)
        : m_path(path), m_folder(folder), m_types(types) {}

    QByteArray name;
    QString destination;

protected:
    void run() override
    {
        const QFileInfo source(m_path);
        QString base = source.fileName();
        if (!source.isAbsolute() || !source.isFile() || base.isEmpty() ||
            base == QStringLiteral(".") || base == QStringLiteral("..") ||
            base.contains(QLatin1Char('\n')) || base.contains(QLatin1Char('\r')) ||
            base.contains(QLatin1Char('\\')) || !matchesType(base, m_types)) {
            qWarning() << "Picker: selected file is invalid or does not match the requested MIME types";
            return;
        }
        const QDir folder(m_folder);
        if (!folder.isAbsolute() || !folder.exists()) {
            qWarning() << "Picker: request folder is unavailable";
            return;
        }
        QString candidate = base;
        for (int i = 1; QFileInfo::exists(folder.filePath(candidate)); ++i) {
            if (i > 10000) {
                qWarning() << "Picker: cannot find a unique destination name";
                return;
            }
            candidate = QString::number(i) + QLatin1Char('_') + base;
        }
        destination = folder.filePath(candidate);
        if (!QFile::copy(m_path, destination)) {
            destination.clear();
            qWarning() << "Picker: copying selected file failed";
            return;
        }
        name = candidate.toUtf8();
    }

private:
    QString m_path;
    QString m_folder;
    QStringList m_types;
};

AuroraPickerHost::AuroraPickerHost(QObject *parent) : QObject(parent) {}

AuroraPickerHost::~AuroraPickerHost()
{
    stop();
}

megler_system_host AuroraPickerHost::host()
{
    megler_system_host result = {};
    result.user = this;
    result.pick = pick;
    result.permission = permission;
    return result;
}

int MEGLER_CB AuroraPickerHost::permission(void *user, const char *group, bool)
{
    const AuroraPickerHost *self = static_cast<AuroraPickerHost *>(user);
    if (!self || !group) return MEGLER_PERMISSION_DENIED_FIXED;
    return self->m_deniedPermissions.contains(QString::fromUtf8(group))
        ? MEGLER_PERMISSION_DENIED_FIXED : MEGLER_PERMISSION_GRANTED;
}

bool MEGLER_CB AuroraPickerHost::pick(void *user, const megler_pick_request *request)
{
    AuroraPickerHost *self = static_cast<AuroraPickerHost *>(user);
    if (!self || !request) return false;
    QMutexLocker lock(&self->m_mutex);
    if (!self->m_ready || self->m_active || request->kind != MEGLER_PICK_FILES ||
        request->multiple || !request->folder || !request->folder[0]) {
        qWarning() << "Picker: unavailable, busy, or unsupported request";
        return false;
    }
    self->m_active = true;
    ++self->m_serial;
    self->m_id = request->id;
    self->m_folder = QString::fromUtf8(request->folder);
    self->m_types = QString::fromUtf8(request->types ? request->types : "")
            .split(QLatin1Char('\n'), QString::SkipEmptyParts);
    if (!QMetaObject::invokeMethod(self, "dispatch", Qt::QueuedConnection,
                                   Q_ARG(QString, QString::number(self->m_serial)))) {
        self->m_active = false;
        qWarning() << "Picker: cannot dispatch request";
        return false;
    }
    return true;
}

void AuroraPickerHost::dispatch(const QString &token)
{
    QStringList types;
    {
        QMutexLocker lock(&m_mutex);
        if (!m_active || !m_ready || token != QString::number(m_serial)) return;
        types = m_types;
    }
    QStringList filters;
    if (!filtersFor(types, &filters)) {
        qWarning() << "Picker: MIME filter is not supported";
        finish(MEGLER_PICK_FAILED);
        return;
    }
    emit showPicker(token, filters);
}

void AuroraPickerHost::setUiReady(bool ready)
{
    if (!ready) {
        stop();
        return;
    }
    QMutexLocker lock(&m_mutex);
    m_ready = true;
}

void AuroraPickerHost::selected(const QString &token, const QString &path)
{
    QString folder;
    QStringList types;
    {
        QMutexLocker lock(&m_mutex);
        if (!m_active || token != QString::number(m_serial) || m_copy) return;
        folder = m_folder;
        types = m_types;
    }
    m_copy = new PickerCopyThread(path, folder, types);
    connect(m_copy, &QThread::finished, this, &AuroraPickerHost::copyFinished);
    m_copy->start();
}

void AuroraPickerHost::canceled(const QString &token)
{
    {
        QMutexLocker lock(&m_mutex);
        if (!m_active || token != QString::number(m_serial) || m_copy) return;
    }
    finish(MEGLER_PICK_CANCELED);
}

void AuroraPickerHost::showFailed(const QString &token)
{
    {
        QMutexLocker lock(&m_mutex);
        if (!m_active || token != QString::number(m_serial) || m_copy) return;
    }
    qWarning() << "Picker: cannot show picker page";
    finish(MEGLER_PICK_FAILED);
}

void AuroraPickerHost::copyFinished()
{
    if (!m_copy || sender() != m_copy) return;
    const QByteArray name = m_copy->name;
    delete m_copy;
    m_copy = nullptr;
    finish(name.isEmpty() ? MEGLER_PICK_FAILED : MEGLER_PICK_OK, name);
}

void AuroraPickerHost::finish(int status, const QByteArray &name)
{
    unsigned id;
    {
        QMutexLocker lock(&m_mutex);
        if (!m_active) return;
        id = m_id;
        m_active = false;
    }
    const char *names[] = { name.constData() };
    megler_pick_done(id, status, status == MEGLER_PICK_OK ? 1 : 0,
                     status == MEGLER_PICK_OK ? names : nullptr);
}

void AuroraPickerHost::stop()
{
    {
        QMutexLocker lock(&m_mutex);
        m_ready = false;
    }
    if (m_copy) {
        m_copy->wait();
        if (!m_copy->destination.isEmpty() && !QFile::remove(m_copy->destination))
            qWarning() << "Picker: could not remove canceled copy";
        delete m_copy;
        m_copy = nullptr;
    }
    finish(MEGLER_PICK_CANCELED);
}
