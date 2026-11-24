#include "../shared_folders.h"
#include "../../core/include/megler.h"
#include "../../core/src/apk/mg_apk.h"
#include "../../core/src/bionic/mg_bionic.h"
#include "../../core/src/drivers/mg_driver.h"
#include "../../core/src/drivers/mg_java.h"
#include "../../core/src/run/mg_run.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <thread>
#include <unistd.h>

static std::atomic<bool> mediaPassed(false);

static void check(bool ok, const char *message)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

static void logMessage(int, const char *message, void *)
{
    std::puts(message);
    if (std::strstr(message, "CAMERA_MEDIASTORE_DEDUP_OK")) mediaPassed.store(true);
}

template<class T> static T guest(const char *name)
{
    void *value = mg_bionic_find(name);
    check(value != nullptr, name);
    return reinterpret_cast<T>(value);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    check(argc == 1 || argc == 3, "usage: camera_storage_core_test [fixture.apk android-root]");
    QTemporaryDir temp;
    check(temp.isValid(), "isolated storage");
    qputenv("HOME", temp.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", (temp.path() + QStringLiteral("/config")).toUtf8());
    check(QDir().mkpath(temp.path() + QStringLiteral("/config")), "isolated configuration");
    const QString pictures = temp.path() + QStringLiteral("/Pictures");
    QFile dirs(temp.path() + QStringLiteral("/config/user-dirs.dirs"));
    check(dirs.open(QIODevice::WriteOnly), "user dirs configuration");
    const QByteArray setting = "XDG_PICTURES_DIR=\"" + pictures.toUtf8() + "\"\n";
    check(dirs.write(setting) == setting.size(), "isolated Pictures location");
    dirs.close();

    const QByteArray data = (temp.path() + QStringLiteral("/runtime")).toUtf8();
    megler_config config = {};
    config.data_dir = data.constData();
    config.log = logMessage;
    check(megler_init(&config), "actual native core initialization");
    AuroraStorageHost storage{QStringList()};
    QString error;
    check(storage.configureCameraPictures(QStringLiteral("org.example.camera"), true, &error), "scoped Pictures");
    const megler_storage_host host = storage.host();
    check(megler_set_camera_storage_alias("org.example.camera", true), "request camera alias");
    mg_run_files(data.constData(), data.constData(), "org.example.camera", &host);

    auto openFile = guest<int (*)(const char *, int, ...)>("open");
    auto mkdirFile = guest<int (*)(const char *, unsigned)>("mkdir");
    auto writeFile = guest<long (*)(int, const void *, unsigned long)>("write");
    auto closeFile = guest<int (*)(int)>("close");
    auto statFile = guest<int (*)(const char *, void *)>("stat");
    auto realPath = guest<char *(*)(const char *, char *)>("realpath");
    check(mkdirFile("/sdcard/DCIM", 0777) == 0 &&
          mkdirFile("/sdcard/DCIM/OpenCamera", 0777) == 0, "actual guest camera mkdir");
    const QByteArray png = QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aKQAAAABJRU5ErkJggg==");
    const int fd = openFile("/sdcard/DCIM/OpenCamera/fixture.png", O_WRONLY | O_CREAT | O_EXCL, 0666);
    check(fd >= 0 && writeFile(fd, png.constData(), png.size()) == png.size() && closeFile(fd) == 0,
          "actual guest camera open/write/close");
    const QString scoped = pictures + QStringLiteral("/AndroidCompatibilityLayer/org.example.camera");
    QFile saved(scoped + QStringLiteral("/DCIM/OpenCamera/fixture.png"));
    check(saved.open(QIODevice::ReadOnly) && saved.readAll() == png, "photo immediately exists in native Pictures");
    saved.close();

    char first[4096] = {}, second[4096] = {}, third[4096] = {};
    check(realPath("/sdcard/DCIM/OpenCamera/fixture.png", first) &&
          realPath("/storage/emulated/0/Pictures/DCIM/OpenCamera/fixture.png", second) &&
          realPath("/mnt/sdcard/dcim/OpenCamera/fixture.png", third) &&
          std::strcmp(first, second) == 0 && std::strcmp(first, third) == 0 &&
          std::strcmp(first, "/storage/emulated/0/Pictures/DCIM/OpenCamera/fixture.png") == 0,
          "actual realpath aliases have one identity");
    unsigned long long firstStat[32] = {}, secondStat[32] = {};
    check(statFile("/sdcard/DCIM/OpenCamera/fixture.png", firstStat) == 0 &&
          statFile("/sdcard/Pictures/DCIM/OpenCamera/fixture.png", secondStat) == 0 &&
          firstStat[0] == secondStat[0] && firstStat[1] == secondStat[1],
          "actual stat device and inode agree");
    const int canonical = openFile("/sdcard/Pictures/DCIM/OpenCamera/fixture.png", O_RDONLY);
    check(canonical >= 0 && closeFile(canonical) == 0, "actual canonical alias opens the same file");
    const int rootDirectory = openFile("/sdcard/DCIM-other", O_RDONLY | O_DIRECTORY);
    check(rootDirectory < 0, "component prefix is not a camera alias");
    check(openFile("/sdcard/DCIM/../../Pictures/../../outside/secret.png", O_RDONLY) < 0,
          "guest traversal cannot leave the scoped Pictures tree");
    const QString outside = temp.path() + QStringLiteral("/outside");
    check(QDir().mkpath(outside), "synthetic outside folder");
    QFile secret(outside + QStringLiteral("/secret.png"));
    check(secret.open(QIODevice::WriteOnly) && secret.write(png) == png.size(), "synthetic outside file");
    secret.close();
    const QByteArray escape = QFile::encodeName(scoped + QStringLiteral("/DCIM/escape"));
    check(::symlink(QFile::encodeName(outside).constData(), escape.constData()) == 0, "synthetic native symlink");
    check(openFile("/sdcard/DCIM/escape/secret.png", O_RDONLY) < 0 &&
          statFile("/sdcard/Pictures/DCIM/escape/secret.png", firstStat) < 0,
          "actual guest open/stat reject symlink escape");

    AuroraStorageHost denied{QStringList()};
    check(denied.configureCameraPictures(QStringLiteral("org.example.camera"), false, &error), "denied Pictures");
    const megler_storage_host deniedHost = denied.host();
    check(megler_set_camera_storage_alias("org.example.camera", true), "request denied alias");
    mg_run_files(data.constData(), data.constData(), "org.example.camera", &deniedHost);
    check(openFile("/sdcard/DCIM/denied.png", O_WRONLY | O_CREAT, 0666) < 0 &&
          mkdirFile("/sdcard/DCIM/denied", 0777) < 0 &&
          !QFile::exists(QString::fromUtf8(data) + QStringLiteral("/sdcard/DCIM/denied.png")) &&
          !QFile::exists(QString::fromUtf8(data) + QStringLiteral("/sdcard/Pictures/DCIM/denied.png")),
          "permission denial has no successful private fallback");
    const megler_storage_host missing = {};
    check(megler_set_camera_storage_alias("org.example.camera", true), "request alias with missing link");
    mg_run_files(data.constData(), data.constData(), "org.example.camera", &missing);
    check(openFile("/sdcard/DCIM/missing.png", O_WRONLY | O_CREAT, 0666) < 0, "missing Pictures link is fail closed");
    check(megler_set_camera_storage_alias("org.example.camera", true), "request package-bound alias");
    mg_run_files(data.constData(), data.constData(), "org.example.other", &host);
    check(openFile("/sdcard/DCIM/OpenCamera/fixture.png", O_RDONLY) < 0 &&
          mkdirFile("/sdcard/DCIM/wrong-package", 0777) < 0 &&
          openFile("/sdcard/Pictures/DCIM/wrong-package.png", O_WRONLY | O_CREAT, 0666) < 0,
          "wrong package cannot read or write the camera mount and has no private fallback");
    mg_run_files(data.constData(), data.constData(), "org.example.other", &missing);
    check(mkdirFile("/sdcard/DCIM/Other", 0777) == 0 &&
          realPath("/sdcard/DCIM/Other", first) &&
          std::strcmp(first, "/storage/emulated/0/DCIM/Other") == 0,
          "camera alias does not leak into the next package");
    check(!megler_set_camera_storage_alias(nullptr, true), "invalid package is explicitly rejected");
    std::puts("Actual camera storage open/stat/realpath, denial and package isolation OK");
    if (argc == 1) return 0;

    check(megler_set_camera_storage_alias("org.example.camera", true), "restore camera fixture alias");
    mg_run_files(data.constData(), data.constData(), "org.example.camera", &host);
    mg_apk_info info = {};
    check(mg_apk_read_info(argv[1], &info) && std::strcmp(info.package, "org.example.camera") == 0,
          "verified private MediaStore fixture package");
    megler_run_config run = {};
    run.apk_path = argv[1];
    run.android_root = argv[2];
    run.width = 800;
    run.height = 480;
    run.density_dpi = 160;
    run.storage = host;
    check(mg_driver_java.start(&run, &info), "actual framework starts with private fixture DEX");
    for (int i = 0; i != 1200; ++i) {
        if (mg_java_ended(nullptr, nullptr)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    mg_driver_java.stop();
    check(mediaPassed.load(), "actual MediaStore scan/query/open, refresh, delete, rescan and managed insert");
    mg_apk_info_free(&info);
    std::puts("Actual MediaStore deduplication and stable ContentResolver URI OK");
    std::exit(0);
}
