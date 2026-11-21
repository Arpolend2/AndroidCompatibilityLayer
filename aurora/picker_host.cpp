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
#include <QTemporaryDir>

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
    PickerCopyThread(const QStringList &paths, const QString &folder, const QStringList &types)
        : m_paths(paths), m_folder(folder), m_types(types) {}

    QList<QByteArray> names;
    QStringList destinations;

    void removeCopies()
    {
        for (const QString &destination : destinations)
            if (!QFile::remove(destination)) qWarning() << "Picker: could not remove canceled copy";
        destinations.clear();
        names.clear();
    }

protected:
    void run() override
    {
        for (const QString &path : m_paths) {
            const QFileInfo source(path);
            const QString base = source.fileName();
            if (!source.isAbsolute() || !source.isFile() || base.isEmpty() ||
                base == QStringLiteral(".") || base == QStringLiteral("..") ||
                base.contains(QLatin1Char('\n')) || base.contains(QLatin1Char('\r')) ||
                base.contains(QLatin1Char('\\')) || !matchesType(base, m_types)) {
                qWarning() << "Picker: selected file is invalid or does not match the requested MIME types";
                removeCopies();
                return;
            }
            const QDir folder(m_folder);
            if (!folder.isAbsolute() || !folder.exists()) {
                qWarning() << "Picker: request folder is unavailable";
                removeCopies();
                return;
            }
            QString candidate = base;
            for (int i = 1; QFileInfo::exists(folder.filePath(candidate)); ++i) {
                if (i > 10000) {
                    qWarning() << "Picker: cannot find a unique destination name";
                    removeCopies();
                    return;
                }
                candidate = QString::number(i) + QLatin1Char('_') + base;
            }
            const QString destination = folder.filePath(candidate);
            if (!QFile::copy(path, destination)) {
                qWarning() << "Picker: copying selected file failed";
                removeCopies();
                return;
            }
            destinations.append(destination);
            names.append(candidate.toUtf8());
        }
    }

private:
    QStringList m_paths;
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
    if (!self->m_ready || self->m_active ||
        (request->kind != MEGLER_PICK_FILES && request->kind != MEGLER_PICK_PHOTO) ||
        (request->kind == MEGLER_PICK_FILES && request->multiple && !self->m_supportsMultiple) ||
        (request->kind == MEGLER_PICK_PHOTO && !self->m_supportsPhoto) ||
        (request->kind == MEGLER_PICK_PHOTO && self->m_deniedPermissions.contains(QStringLiteral("camera"))) ||
        !request->folder || !request->folder[0] || request->max < 0) {
        qWarning() << "Picker: unavailable, busy, or unsupported request";
        return false;
    }
    self->m_active = true;
    ++self->m_serial;
    self->m_id = request->id;
    self->m_kind = request->kind;
    self->m_multiple = request->multiple;
    self->m_max = request->max;
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

void AuroraPickerHost::setPickerCapabilities(bool multiple, bool photo)
{
    QMutexLocker lock(&m_mutex);
    m_supportsMultiple = multiple;
    m_supportsPhoto = photo;
}

void AuroraPickerHost::dispatch(const QString &token)
{
    QStringList types;
    bool multiple;
    int kind;
    {
        QMutexLocker lock(&m_mutex);
        if (!m_active || !m_ready || token != QString::number(m_serial)) return;
        types = m_types;
        multiple = m_multiple;
        kind = m_kind;
    }
    if (kind == MEGLER_PICK_PHOTO) {
        if (receivers(SIGNAL(showPhoto(QString,QString))) == 0) {
            qWarning() << "Picker: photo UI is not connected";
            finish(MEGLER_PICK_FAILED);
            return;
        }
        m_photoTemp = new QTemporaryDir;
        if (!m_photoTemp->isValid()) {
            qWarning() << "Picker: cannot create private camera directory";
            delete m_photoTemp;
            m_photoTemp = nullptr;
            finish(MEGLER_PICK_FAILED);
            return;
        }
        const QString privateDir = QFileInfo(m_photoTemp->path()).canonicalFilePath();
        if (privateDir.isEmpty()) {
            qWarning() << "Picker: cannot resolve private camera directory";
            finish(MEGLER_PICK_FAILED);
            return;
        }
        m_photoPath = QDir(privateDir).absoluteFilePath(QStringLiteral("photo.jpg"));
        m_photoSaved = false;
        emit showPhoto(token, m_photoPath);
        return;
    }
    QStringList filters;
    if (!filtersFor(types, &filters)) {
        qWarning() << "Picker: MIME filter is not supported";
        finish(MEGLER_PICK_FAILED);
        return;
    }
    emit showPicker(token, filters, multiple);
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
    selectedMany(token, QStringList() << path);
}

void AuroraPickerHost::selectedMany(const QString &token, const QStringList &paths)
{
    QString folder;
    QStringList types;
    {
        QMutexLocker lock(&m_mutex);
        if (!m_active || m_kind != MEGLER_PICK_FILES || token != QString::number(m_serial) || m_copy) return;
        if (paths.isEmpty() || (!m_multiple && paths.size() != 1) ||
            (m_max > 0 && paths.size() > m_max)) {
            qWarning() << "Picker: invalid selection count";
            lock.unlock();
            finish(MEGLER_PICK_FAILED);
            return;
        }
        folder = m_folder;
        types = m_types;
    }
    m_copy = new PickerCopyThread(paths, folder, types);
    connect(m_copy, &QThread::finished, this, &AuroraPickerHost::copyFinished);
    m_copy->start();
}

void AuroraPickerHost::validatePhoto(const QString &token, const QString &path)
{
    bool valid = false;
    {
        QMutexLocker lock(&m_mutex);
        if (!m_active || m_kind != MEGLER_PICK_PHOTO || token != QString::number(m_serial) ||
            !m_photoTemp || m_copy) return;
        const QFileInfo file(path);
        if (path == m_photoPath && file.isFile() && !file.isSymLink() &&
            file.canonicalFilePath() == m_photoPath) {
            QFile input(path);
            valid = input.open(QIODevice::ReadOnly) && input.read(3) == QByteArray::fromHex("ffd8ff");
        }
        m_photoSaved = valid;
    }
    emit photoValidated(token, valid);
}

void AuroraPickerHost::confirmPhoto(const QString &token)
{
    QString path, folder;
    {
        QMutexLocker lock(&m_mutex);
        if (!m_active || m_kind != MEGLER_PICK_PHOTO || token != QString::number(m_serial) ||
            !m_photoSaved || !m_photoTemp || m_copy) return;
        path = m_photoPath;
        folder = m_folder;
    }
    m_copy = new PickerCopyThread(QStringList() << path, folder, QStringList() << QStringLiteral("image/jpeg"));
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
    const QList<QByteArray> names = m_copy->names;
    delete m_copy;
    m_copy = nullptr;
    finish(names.isEmpty() ? MEGLER_PICK_FAILED : MEGLER_PICK_OK, names);
}

void AuroraPickerHost::finish(int status, const QList<QByteArray> &names)
{
    unsigned id;
    {
        QMutexLocker lock(&m_mutex);
        if (!m_active) return;
        id = m_id;
        m_active = false;
    }
    delete m_photoTemp;
    m_photoTemp = nullptr;
    m_photoPath.clear();
    m_photoSaved = false;
    QVector<const char *> pointers;
    if (status == MEGLER_PICK_OK)
        for (const QByteArray &name : names) pointers.append(name.constData());
    megler_pick_done(id, status, pointers.size(), pointers.isEmpty() ? nullptr : pointers.constData());
}

void AuroraPickerHost::stop()
{
    QString photoToken;
    {
        QMutexLocker lock(&m_mutex);
        m_ready = false;
        if (m_active && m_kind == MEGLER_PICK_PHOTO)
            photoToken = QString::number(m_serial);
    }
    if (!photoToken.isEmpty()) emit closePhoto(photoToken);
    if (m_copy) {
        m_copy->wait();
        m_copy->removeCopies();
        delete m_copy;
        m_copy = nullptr;
    }
    finish(MEGLER_PICK_CANCELED);
}
