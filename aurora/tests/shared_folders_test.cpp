#include "../shared_folders.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QVariantMap>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

static void check(bool ok, const char *description)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        std::exit(1);
    }
}

static void writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    check(file.open(QIODevice::WriteOnly), "open fixture");
    check(file.write(contents) == contents.size(), "write fixture");
}

static int descriptor(void *handle)
{
    return static_cast<int>(reinterpret_cast<intptr_t>(handle) - 1);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("AndroidCompatibilityLayerTest"));
    QCoreApplication::setApplicationName(QStringLiteral("SharedFolders"));
    QTemporaryDir temporary;
    check(temporary.isValid(), "temporary root");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    const QString root = temporary.path() + QStringLiteral("/Documents");
    const QString outside = temporary.path() + QStringLiteral("/outside");
    check(QDir().mkpath(root + QStringLiteral("/nested")) && QDir().mkpath(outside), "create folders");
    writeFile(root + QStringLiteral("/nested/read.txt"), "readable");
    writeFile(outside + QStringLiteral("/secret.txt"), "private");
    const QByteArray outsidePath = QFile::encodeName(outside);
    const QByteArray escapePath = QFile::encodeName(root + QStringLiteral("/escape"));
    check(::symlink(outsidePath.constData(), escapePath.constData()) == 0, "create escape link");

    QVector<AuroraStorageHost::Folder> folders;
    folders.append(AuroraStorageHost::Folder{QStringLiteral("Documents"), root});
    AuroraStorageHost storage(folders);
    megler_storage_host host = storage.host();
    check(host.link_count == 1 && host.links[0].writable, "publish selected folder");

    int error = -1;
    void *handle = host.open(host.user, 0, "nested/read.txt", MEGLER_STORAGE_READ,
                             MEGLER_STORAGE_FILE, &error);
    check(handle && error == MEGLER_STORAGE_OK, "open a linked file");
    char content[16] = {};
    check(::read(descriptor(handle), content, sizeof(content)) == 8 &&
          std::strcmp(content, "readable") == 0, "read linked file");
    ::close(descriptor(handle));

    handle = host.open(host.user, 0, "../outside/secret.txt", MEGLER_STORAGE_READ,
                       MEGLER_STORAGE_FILE, &error);
    check(!handle && error == MEGLER_STORAGE_INVALID, "reject parent traversal");
    handle = host.open(host.user, 0, "escape/secret.txt", MEGLER_STORAGE_READ,
                       MEGLER_STORAGE_FILE, &error);
    check(!handle && error == MEGLER_STORAGE_DENIED, "reject symlink escape");

    check(host.make_folder(host.user, 0, "created", &error) && error == MEGLER_STORAGE_OK,
          "create a linked folder");
    handle = host.open(host.user, 0, "created/new.txt",
                       MEGLER_STORAGE_WRITE | MEGLER_STORAGE_CREATE | MEGLER_STORAGE_EXCLUSIVE,
                       MEGLER_STORAGE_FILE, &error);
    check(handle && error == MEGLER_STORAGE_OK, "create a linked file");
    check(::write(descriptor(handle), "data", 4) == 4, "write linked file");
    ::close(descriptor(handle));
    check(host.rename(host.user, 0, "created/new.txt", 0, "created/renamed.txt", &error),
          "rename linked file");
    check(host.remove(host.user, 0, "created/renamed.txt", false, &error),
          "remove linked file");
    check(!host.remove(host.user, 0, "", true, &error) &&
          error == MEGLER_STORAGE_DENIED, "protect linked folder root");
    QFile secret(outside + QStringLiteral("/secret.txt"));
    check(secret.open(QIODevice::ReadOnly) && secret.readAll() == "private",
          "leave outside data untouched");

    QSettings().setValue(QStringLiteral("storage/sharedFolders"), QStringList());
    const QStringList standardNames = QStringList()
        << QStringLiteral("Documents") << QStringLiteral("Download") << QStringLiteral("Pictures")
        << QStringLiteral("Movies") << QStringLiteral("Music") << QStringLiteral("Unknown");
    AuroraStorageHost namedStorage(standardNames);
    const megler_storage_host namedHost = namedStorage.host();
    int expectedLinks = 0;
    const QVariantList options = AuroraStorageHost::options();
    for (const QVariant &entry : options)
        if (entry.toMap().value(QStringLiteral("available")).toBool()) ++expectedLinks;
    check(namedHost.link_count == expectedLinks, "only available standard names are linked");
    for (int i = 0; i < namedHost.link_count; ++i)
        check(QString::fromUtf8(namedHost.links[i].name) != QLatin1String("Unknown"),
              "unknown standard name is excluded");

    QVector<AuroraStorageHost::Folder> missingFolders;
    missingFolders.append(AuroraStorageHost::Folder{
        QStringLiteral("Documents"), temporary.path() + QStringLiteral("/missing")});
    AuroraStorageHost missingStorage(missingFolders);
    check(missingStorage.host().link_count == 0, "missing folder is excluded");

    std::puts("Aurora shared folders OK");
    return 0;
}
