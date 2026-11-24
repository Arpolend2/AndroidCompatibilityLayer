#include "../shared_folders.h"
#include "../../core/src/bionic/mg_camera_storage_path.h"

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
    qputenv("HOME", temporary.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", (temporary.path() + QStringLiteral("/config")).toUtf8());
    check(QDir().mkpath(temporary.path() + QStringLiteral("/config")), "create isolated user configuration");
    const QString pictures = temporary.path() + QStringLiteral("/Pictures");
    writeFile(temporary.path() + QStringLiteral("/config/user-dirs.dirs"),
              "XDG_PICTURES_DIR=\"" + pictures.toUtf8() + "\"\n");
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

    AuroraStorageHost camera{QStringList()};
    QString cameraError;
    check(camera.configureCameraPictures(QStringLiteral("org.example.camera"), true, &cameraError) &&
          cameraError.isEmpty(), "prepare package-scoped Pictures");
    const megler_storage_host cameraHost = camera.host();
    check(cameraHost.link_count == 1 && std::strcmp(cameraHost.links[0].name, "Pictures") == 0,
          "publish one canonical Pictures link, not duplicate DCIM");
    check(cameraHost.make_folder(cameraHost.user, 0, "DCIM", &error) &&
          cameraHost.make_folder(cameraHost.user, 0, "DCIM/OpenCamera", &error), "create camera destination");
    handle = cameraHost.open(cameraHost.user, 0, "DCIM/OpenCamera/synthetic.jpg",
                             MEGLER_STORAGE_WRITE | MEGLER_STORAGE_CREATE | MEGLER_STORAGE_EXCLUSIVE,
                             MEGLER_STORAGE_FILE, &error);
    check(handle && error == MEGLER_STORAGE_OK, "save camera fixture directly in native Pictures");
    check(::write(descriptor(handle), "synthetic", 9) == 9, "write camera fixture");
    ::close(descriptor(handle));
    const QString cameraRoot = pictures + QStringLiteral("/AndroidCompatibilityLayer/org.example.camera");
    QFile saved(cameraRoot + QStringLiteral("/DCIM/OpenCamera/synthetic.jpg"));
    check(saved.open(QIODevice::ReadOnly) && saved.readAll() == "synthetic",
          "camera file exists in package-scoped native Pictures");
    handle = cameraHost.open(cameraHost.user, 0, "../org.example.other/private.jpg",
                             MEGLER_STORAGE_READ, MEGLER_STORAGE_FILE, &error);
    check(!handle && error == MEGLER_STORAGE_INVALID, "camera mount cannot traverse to another package");
    const QByteArray cameraEscape = QFile::encodeName(cameraRoot + QStringLiteral("/escape"));
    check(::symlink(outsidePath.constData(), cameraEscape.constData()) == 0, "create camera escape fixture");
    handle = cameraHost.open(cameraHost.user, 0, "escape/secret.txt", MEGLER_STORAGE_READ,
                             MEGLER_STORAGE_FILE, &error);
    check(!handle && error == MEGLER_STORAGE_DENIED, "camera mount rejects symlink escape");

    AuroraStorageHost denied{QStringList()};
    check(denied.configureCameraPictures(QStringLiteral("org.example.camera"), false, &cameraError),
          "prepare explicit permission-denied mount");
    const megler_storage_host deniedHost = denied.host();
    check(deniedHost.link_count == 1 && !deniedHost.links[0].writable, "denial keeps mount instead of private fallback");
    handle = deniedHost.open(deniedHost.user, 0, "DCIM/new.jpg",
                             MEGLER_STORAGE_WRITE | MEGLER_STORAGE_CREATE,
                             MEGLER_STORAGE_FILE, &error);
    check(!handle && error == MEGLER_STORAGE_DENIED, "camera write reports permission denial");
    check(!deniedHost.make_folder(deniedHost.user, 0, "DCIM", &error) &&
          error == MEGLER_STORAGE_DENIED, "camera mkdir reports permission denial");
    check(!camera.configureCameraPictures(QStringLiteral("../outside"), true, &cameraError) &&
          !cameraError.isEmpty(), "reject untrusted package path");

    const QByteArray badScope = QFile::encodeName(pictures + QStringLiteral("/AndroidCompatibilityLayer/org.example.bad"));
    check(::symlink(outsidePath.constData(), badScope.constData()) == 0, "create redirected package fixture");
    AuroraStorageHost redirected{QStringList()};
    check(!redirected.configureCameraPictures(QStringLiteral("org.example.bad"), true, &cameraError) &&
          !cameraError.isEmpty(), "refuse redirected package root explicitly");

    QVector<AuroraStorageHost::Folder> explicitPictures;
    explicitPictures.append(AuroraStorageHost::Folder{QStringLiteral("Pictures"), pictures});
    AuroraStorageHost selected(explicitPictures);
    check(selected.configureCameraPictures(QStringLiteral("org.example.camera"), true, &cameraError),
          "keep explicitly selected Pictures");
    const megler_storage_host selectedHost = selected.host();
    handle = selectedHost.open(selectedHost.user, 0, "AndroidCompatibilityLayer/org.example.camera/DCIM/OpenCamera/synthetic.jpg",
                               MEGLER_STORAGE_READ, MEGLER_STORAGE_FILE, &error);
    check(handle && selectedHost.link_count == 1, "selected Pictures keeps its original root and one link");
    ::close(descriptor(handle));
    check(selected.configureCameraPictures(QStringLiteral("org.example.camera"), false, &cameraError),
          "deny camera writes without discarding selected Pictures reads");
    const megler_storage_host readOnlyHost = selected.host();
    handle = readOnlyHost.open(readOnlyHost.user, 0, "AndroidCompatibilityLayer/org.example.camera/DCIM/OpenCamera/synthetic.jpg",
                               MEGLER_STORAGE_READ, MEGLER_STORAGE_FILE, &error);
    check(handle && !readOnlyHost.links[0].writable, "explicit Pictures remains readable without write permission");
    ::close(descriptor(handle));
    handle = readOnlyHost.open(readOnlyHost.user, 0, "new.jpg", MEGLER_STORAGE_WRITE | MEGLER_STORAGE_CREATE,
                               MEGLER_STORAGE_FILE, &error);
    check(!handle && error == MEGLER_STORAGE_DENIED, "read-only host rejects writes itself");
    check(!readOnlyHost.make_folder(readOnlyHost.user, 0, "new", &error) &&
          error == MEGLER_STORAGE_DENIED, "read-only host rejects mkdir");
    check(!readOnlyHost.remove(readOnlyHost.user, 0, "AndroidCompatibilityLayer", true, &error) &&
          error == MEGLER_STORAGE_DENIED, "read-only host rejects removal");
    check(!readOnlyHost.rename(readOnlyHost.user, 0, "AndroidCompatibilityLayer", 0, "moved", &error) &&
          error == MEGLER_STORAGE_DENIED, "read-only host rejects rename");

    check(std::strcmp(mg_camera_storage_suffix("/storage/emulated/0/DCIM/OpenCamera/photo.jpg"),
                      "/OpenCamera/photo.jpg") == 0 &&
          std::strcmp(mg_camera_storage_suffix("/storage/emulated/0/Pictures/DCIM/OpenCamera/photo.jpg"),
                      "/OpenCamera/photo.jpg") == 0, "DCIM aliases share a canonical suffix");
    check(!mg_camera_storage_suffix("/storage/emulated/0/DCIM-other/photo.jpg") &&
          !mg_camera_storage_suffix("/storage/emulated/0/Pictures-other/DCIM/photo.jpg") &&
          !mg_camera_storage_suffix("/storage/emulated/10/DCIM/photo.jpg"),
          "camera alias respects path component and user boundaries");

    std::puts("Aurora shared folders OK");
    return 0;
}
