#include "shared_folders.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>
#include <QVariantMap>

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

bool withinRoot(const QString &root, const QString &path)
{
    return path == root || path.startsWith(root + QLatin1Char('/'));
}

bool validFolderName(const QString &name)
{
    if (name.isEmpty() || name == QLatin1String("Android")) return false;
    for (QChar ch : name) {
        const ushort value = ch.unicode();
        if (!(ch.isLetterOrNumber() || ch == QLatin1Char('_') || ch == QLatin1Char('-') || ch == QLatin1Char('.')) ||
            value < 32)
            return false;
    }
    return name != QLatin1String(".") && name != QLatin1String("..");
}

int storageError(int value)
{
    switch (value) {
    case 0: return MEGLER_STORAGE_OK;
    case ENOENT: return MEGLER_STORAGE_NOT_FOUND;
    case EACCES:
    case EPERM: return MEGLER_STORAGE_DENIED;
    case EEXIST: return MEGLER_STORAGE_EXISTS;
    case ENOTDIR: return MEGLER_STORAGE_NOT_A_FOLDER;
    case EISDIR: return MEGLER_STORAGE_IS_A_FOLDER;
    case ENOSPC: return MEGLER_STORAGE_FULL;
    case EINVAL: return MEGLER_STORAGE_INVALID;
    case ENOTEMPTY: return MEGLER_STORAGE_NOT_EMPTY;
    case EBUSY: return MEGLER_STORAGE_BUSY;
    case EXDEV: return MEGLER_STORAGE_OTHER_DEVICE;
    default: return MEGLER_STORAGE_FAILED;
    }
}

int pathError(int *error, int code)
{
    if (error) *error = code;
    return code;
}

int errnoError(int *error)
{
    return pathError(error, storageError(errno));
}

bool parseRelativePath(const char *path, QStringList *parts, bool allowRoot)
{
    if (!path) return false;
    const QByteArray bytes(path);
    const QString relative = QString::fromUtf8(bytes.constData(), bytes.size());
    if (relative.toUtf8() != bytes || relative.startsWith(QLatin1Char('/')) ||
        relative.contains(QLatin1Char('\\')) || relative.contains(QLatin1Char(':')))
        return false;
    if (relative.isEmpty()) return allowRoot;
    const QStringList segments = relative.split(QLatin1Char('/'), QString::KeepEmptyParts);
    for (const QString &segment : segments) {
        if (segment.isEmpty() || segment == QLatin1String(".") || segment == QLatin1String("..")) return false;
        for (QChar ch : segment)
            if (ch.unicode() < 32) return false;
    }
    *parts = segments;
    return true;
}

QString canonicalDirectory(const QString &path)
{
    const QFileInfo info(path);
    if (!info.isDir() || info.isSymLink()) return QString();
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? QString() : QDir::cleanPath(canonical);
}

QStringList standardFolderNames()
{
    return QStringList() << QStringLiteral("Documents") << QStringLiteral("Download")
                         << QStringLiteral("Pictures") << QStringLiteral("Movies")
                         << QStringLiteral("Music");
}

} // namespace

struct AuroraStorageHost::Directory {
    explicit Directory(int value) : fd(value) {}
    ~Directory() { ::close(fd); }
    int fd;
};

AuroraStorageHost::AuroraStorageHost()
    : AuroraStorageHost(QSettings().value(QStringLiteral("storage/sharedFolders")).toStringList())
{
}

AuroraStorageHost::AuroraStorageHost(const QStringList &folderNames)
{
    QVector<Folder> folders;
    const QStringList known = standardFolderNames();
    for (const QString &name : folderNames) {
        if (!known.contains(name)) {
            qWarning() << "Ignoring unknown standard shared folder:" << name;
            continue;
        }
        folders.append(Folder{name, systemFolderPath(name)});
    }
    setFolders(folders);
}

AuroraStorageHost::AuroraStorageHost(const QVector<Folder> &folders)
{
    setFolders(folders);
}

void AuroraStorageHost::setFolders(const QVector<Folder> &folders)
{
    for (const Folder &folder : folders) {
        if (!validFolderName(folder.name)) {
            qWarning() << "Ignoring invalid shared folder name:" << folder.name;
            continue;
        }
        const QString root = canonicalDirectory(folder.path);
        if (root.isEmpty()) {
            qWarning() << "Shared folder is unavailable:" << folder.name << folder.path;
            continue;
        }
        bool duplicate = false;
        for (const Link &existing : m_links)
            duplicate = duplicate || existing.name.compare(folder.name, Qt::CaseInsensitive) == 0;
        if (!duplicate) {
            const QByteArray nativeRoot = QFile::encodeName(root);
            const int fd = ::open(nativeRoot.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (fd < 0) {
                qWarning() << "Cannot open shared folder:" << folder.name << storageError(errno);
                continue;
            }
            m_links.append(Link{folder.name, root, true, std::make_shared<Directory>(fd)});
        }
    }
    publishLinks();
}

void AuroraStorageHost::publishLinks()
{
    m_names.clear();
    m_publicLinks.clear();
    for (const Link &link : m_links) {
        m_names.append(link.name.toUtf8());
        megler_storage_link value = {};
        value.name = m_names.last().constData();
        value.writable = link.writable;
        m_publicLinks.append(value);
    }
}

bool AuroraStorageHost::configureCameraPictures(const QString &package, bool allowed, QString *error)
{
    if (error) error->clear();
    bool validPackage = !package.isEmpty() && package.size() < 160 &&
                        package != QLatin1String(".") && package != QLatin1String("..");
    for (QChar ch : package) {
        const ushort c = ch.unicode();
        validPackage = validPackage && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                       (c >= '0' && c <= '9') || c == '_' || c == '.');
    }
    if (!validPackage) {
        if (error) *error = QStringLiteral("Invalid camera storage package.");
        return false;
    }
    if (!allowed) {
        for (Link &link : m_links) {
            if (link.name.compare(QStringLiteral("Pictures"), Qt::CaseInsensitive) == 0) {
                link.writable = false;
                publishLinks();
                return true;
            }
        }
        m_links.append(Link{QStringLiteral("Pictures"), QString(), false, nullptr});
        publishLinks();
        return true;
    }
    for (int i = 0; i < m_links.size(); ++i) {
        Link &link = m_links[i];
        if (link.name.compare(QStringLiteral("Pictures"), Qt::CaseInsensitive) != 0) continue;
        if (link.directory) {
            link.writable = true;
            publishLinks();
            return true;
        }
        m_links.removeAt(i);
        break;
    }

    const QString pictures = systemFolderPath(QStringLiteral("Pictures"));
    if (pictures.isEmpty() || QFileInfo(pictures).isSymLink() ||
        (!QFileInfo(pictures).exists() && !QDir().mkpath(pictures))) {
        if (error) *error = QStringLiteral("Cannot access the system Pictures directory.");
        return false;
    }
    const QString root = canonicalDirectory(pictures);
    if (root.isEmpty()) {
        if (error) *error = QStringLiteral("The system Pictures directory is unavailable.");
        return false;
    }
    const QByteArray nativeRoot = QFile::encodeName(root);
    int fd = ::open(nativeRoot.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (error) *error = QStringLiteral("Cannot open the system Pictures directory.");
        return false;
    }
    QString scoped = root;
    const QStringList parts = QStringList() << QStringLiteral("AndroidCompatibilityLayer") << package;
    for (const QString &part : parts) {
        const QByteArray name = part.toUtf8();
        if (::mkdirat(fd, name.constData(), 0777) != 0 && errno != EEXIST) {
            ::close(fd);
            if (error) *error = QStringLiteral("Cannot create the camera Pictures directory.");
            return false;
        }
        const int next = ::openat(fd, name.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        ::close(fd);
        if (next < 0) {
            if (error) *error = QStringLiteral("Camera Pictures directory access was refused.");
            return false;
        }
        fd = next;
        scoped = QDir(scoped).filePath(part);
    }
    m_links.append(Link{QStringLiteral("Pictures"), scoped, true, std::make_shared<Directory>(fd)});
    publishLinks();
    return true;
}

QVariantList AuroraStorageHost::options()
{
    QVariantList result;
    for (const QString &name : standardFolderNames()) {
        QVariantMap option;
        option.insert(QStringLiteral("name"), name);
        option.insert(QStringLiteral("available"), !canonicalDirectory(systemFolderPath(name)).isEmpty());
        result.append(option);
    }
    return result;
}

QString AuroraStorageHost::systemFolderPath(const QString &name)
{
    if (name == QLatin1String("Documents")) return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (name == QLatin1String("Download")) return QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (name == QLatin1String("Pictures")) return QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (name == QLatin1String("Movies")) return QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    if (name == QLatin1String("Music")) return QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    return QString();
}

bool AuroraStorageHost::resolve(int link, const char *path, bool allowMissingLeaf, QString *resolved, int *error) const
{
    if (error) *error = MEGLER_STORAGE_OK;
    if (!resolved || link < 0 || link >= m_links.size()) {
        pathError(error, MEGLER_STORAGE_NOT_FOUND);
        return false;
    }
    if (!m_links.at(link).directory) {
        pathError(error, MEGLER_STORAGE_DENIED);
        return false;
    }
    QStringList parts;
    if (!parseRelativePath(path, &parts, true)) {
        pathError(error, MEGLER_STORAGE_INVALID);
        return false;
    }
    const QString root = m_links.at(link).root;
    QString current = root;
    for (int i = 0; i < parts.size(); ++i) {
        const QString next = QDir(current).filePath(parts.at(i));
        const QFileInfo info(next);
        if (!info.exists() && !info.isSymLink()) {
            if (allowMissingLeaf && i == parts.size() - 1) {
                *resolved = next;
                return true;
            }
            pathError(error, MEGLER_STORAGE_NOT_FOUND);
            return false;
        }
        if (info.isSymLink()) {
            pathError(error, MEGLER_STORAGE_DENIED);
            return false;
        }
        const QString canonical = info.canonicalFilePath();
        if (canonical.isEmpty() || !withinRoot(root, QDir::cleanPath(canonical))) {
            pathError(error, MEGLER_STORAGE_DENIED);
            return false;
        }
        if (i + 1 < parts.size() && !info.isDir()) {
            pathError(error, MEGLER_STORAGE_NOT_A_FOLDER);
            return false;
        }
        current = QDir::cleanPath(canonical);
    }
    *resolved = current;
    return true;
}

bool AuroraStorageHost::canWrite(int link, int *error) const
{
    if (link < 0 || link >= m_links.size()) {
        pathError(error, MEGLER_STORAGE_NOT_FOUND);
        return false;
    }
    if (!m_links.at(link).writable) {
        pathError(error, MEGLER_STORAGE_DENIED);
        return false;
    }
    return true;
}

int AuroraStorageHost::parentDescriptor(int link, const char *path, QByteArray *leaf, int *error) const
{
    QStringList parts;
    if (!parseRelativePath(path, &parts, true)) {
        pathError(error, MEGLER_STORAGE_INVALID);
        return -1;
    }
    const Link &entry = m_links.at(link);
    if (!entry.directory) {
        pathError(error, MEGLER_STORAGE_DENIED);
        return -1;
    }
    int fd = ::fcntl(entry.directory->fd, F_DUPFD_CLOEXEC, 0);
    if (fd < 0) {
        errnoError(error);
        return -1;
    }
    for (int i = 0; i + 1 < parts.size(); ++i) {
        const QByteArray name = parts.at(i).toUtf8();
        const int next = ::openat(fd, name.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        const int why = errno;
        ::close(fd);
        if (next < 0) {
            pathError(error, storageError(why));
            return -1;
        }
        fd = next;
    }
    *leaf = parts.isEmpty() ? QByteArray(".") : parts.last().toUtf8();
    return fd;
}

void *MEGLER_CB AuroraStorageHost::open(void *user, int link, const char *path, int flags, int kind, int *error)
{
    AuroraStorageHost *self = static_cast<AuroraStorageHost *>(user);
    QString resolved;
    const bool create = kind == MEGLER_STORAGE_FILE && (flags & MEGLER_STORAGE_CREATE);
    if (self && (flags & (MEGLER_STORAGE_WRITE | MEGLER_STORAGE_CREATE | MEGLER_STORAGE_TRUNCATE |
                          MEGLER_STORAGE_APPEND)) && !self->canWrite(link, error))
        return nullptr;
    if (!self || !self->resolve(link, path, create, &resolved, error)) return nullptr;

    int openFlags = O_RDONLY | O_CLOEXEC;
    if (kind == MEGLER_STORAGE_FOLDER) {
        openFlags |= O_DIRECTORY;
    } else if (kind == MEGLER_STORAGE_FILE) {
        if ((flags & MEGLER_STORAGE_READ) && (flags & MEGLER_STORAGE_WRITE)) openFlags = O_RDWR | O_CLOEXEC;
        else if (flags & MEGLER_STORAGE_WRITE) openFlags = O_WRONLY | O_CLOEXEC;
        if (flags & MEGLER_STORAGE_CREATE) openFlags |= O_CREAT;
        if (flags & MEGLER_STORAGE_TRUNCATE) openFlags |= O_TRUNC;
        if (flags & MEGLER_STORAGE_APPEND) openFlags |= O_APPEND;
        if (flags & MEGLER_STORAGE_EXCLUSIVE) openFlags |= O_EXCL;
    } else if (kind != MEGLER_STORAGE_ANY) {
        pathError(error, MEGLER_STORAGE_INVALID);
        return nullptr;
    }

    QByteArray leaf;
    const int parent = self->parentDescriptor(link, path, &leaf, error);
    if (parent < 0) return nullptr;
    const int fd = ::openat(parent, leaf.constData(), openFlags | O_NOFOLLOW, 0666);
    const int why = errno;
    ::close(parent);
    if (fd < 0) {
        pathError(error, storageError(why));
        return nullptr;
    }
    if (error) *error = MEGLER_STORAGE_OK;
    return reinterpret_cast<void *>(static_cast<intptr_t>(fd) + 1);
}

bool MEGLER_CB AuroraStorageHost::makeFolder(void *user, int link, const char *path, int *error)
{
    AuroraStorageHost *self = static_cast<AuroraStorageHost *>(user);
    QString resolved;
    if (!self || !self->canWrite(link, error) || !self->resolve(link, path, true, &resolved, error)) return false;
    QByteArray leaf;
    const int parent = self->parentDescriptor(link, path, &leaf, error);
    if (parent < 0) return false;
    const bool made = ::mkdirat(parent, leaf.constData(), 0777) == 0;
    const int why = errno;
    ::close(parent);
    if (!made) pathError(error, storageError(why));
    else if (error) *error = MEGLER_STORAGE_OK;
    return made;
}

bool MEGLER_CB AuroraStorageHost::remove(void *user, int link, const char *path, bool folder, int *error)
{
    AuroraStorageHost *self = static_cast<AuroraStorageHost *>(user);
    QString resolved;
    if (!self || !self->canWrite(link, error) || !self->resolve(link, path, false, &resolved, error)) return false;
    if (resolved == self->m_links.at(link).root) {
        pathError(error, MEGLER_STORAGE_DENIED);
        return false;
    }
    QByteArray leaf;
    const int parent = self->parentDescriptor(link, path, &leaf, error);
    if (parent < 0) return false;
    const bool removed = ::unlinkat(parent, leaf.constData(), folder ? AT_REMOVEDIR : 0) == 0;
    const int why = errno;
    ::close(parent);
    if (!removed) pathError(error, storageError(why));
    else if (error) *error = MEGLER_STORAGE_OK;
    return removed;
}

bool MEGLER_CB AuroraStorageHost::rename(void *user, int fromLink, const char *from, int toLink, const char *to, int *error)
{
    AuroraStorageHost *self = static_cast<AuroraStorageHost *>(user);
    QString source, destination;
    if (!self || !self->canWrite(fromLink, error) || !self->canWrite(toLink, error) ||
        !self->resolve(fromLink, from, false, &source, error) ||
        !self->resolve(toLink, to, true, &destination, error))
        return false;
    if (source == self->m_links.at(fromLink).root || destination == self->m_links.at(toLink).root) {
        pathError(error, MEGLER_STORAGE_DENIED);
        return false;
    }
    QByteArray sourceLeaf, destinationLeaf;
    const int sourceParent = self->parentDescriptor(fromLink, from, &sourceLeaf, error);
    if (sourceParent < 0) return false;
    const int destinationParent = self->parentDescriptor(toLink, to, &destinationLeaf, error);
    if (destinationParent < 0) {
        ::close(sourceParent);
        return false;
    }
    const bool renamed = ::renameat(sourceParent, sourceLeaf.constData(),
                                   destinationParent, destinationLeaf.constData()) == 0;
    const int why = errno;
    ::close(sourceParent);
    ::close(destinationParent);
    if (!renamed) pathError(error, storageError(why));
    else if (error) *error = MEGLER_STORAGE_OK;
    return renamed;
}

megler_storage_host AuroraStorageHost::host()
{
    megler_storage_host result = {};
    result.user = this;
    result.links = m_publicLinks.constData();
    result.link_count = m_publicLinks.size();
    result.open = &AuroraStorageHost::open;
    result.make_folder = &AuroraStorageHost::makeFolder;
    result.remove = &AuroraStorageHost::remove;
    result.rename = &AuroraStorageHost::rename;
    return result;
}
