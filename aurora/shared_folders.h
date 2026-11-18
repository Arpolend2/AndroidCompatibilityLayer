#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_SHARED_FOLDERS_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_SHARED_FOLDERS_H

#include "../core/include/megler.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVector>

class AuroraStorageHost
{
public:
    struct Folder {
        QString name;
        QString path;
    };

    AuroraStorageHost();
    explicit AuroraStorageHost(const QStringList &folderNames);
    explicit AuroraStorageHost(const QVector<Folder> &folders);

    megler_storage_host host();
    static QVariantList options();

private:
    struct Link {
        QString name;
        QString root;
        bool writable;
    };

    static void *MEGLER_CB open(void *user, int link, const char *path, int flags, int kind, int *error);
    static bool MEGLER_CB makeFolder(void *user, int link, const char *path, int *error);
    static bool MEGLER_CB remove(void *user, int link, const char *path, bool folder, int *error);
    static bool MEGLER_CB rename(void *user, int fromLink, const char *from, int toLink, const char *to, int *error);

    void setFolders(const QVector<Folder> &folders);
    bool resolve(int link, const char *path, bool allowMissingLeaf, QString *resolved, int *error) const;
    static QString systemFolderPath(const QString &name);

    QVector<Link> m_links;
    QVector<QByteArray> m_names;
    QVector<megler_storage_link> m_publicLinks;
};

#endif
