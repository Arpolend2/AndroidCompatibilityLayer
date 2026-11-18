#include "../install_request_snapshot.h"

#include <QFile>

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

static void check(bool condition, const char *what)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

static void writeFile(const std::string &path, const char *contents)
{
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    check(fd >= 0, "create file");
    const size_t size = std::string(contents).size();
    check(write(fd, contents, size) == static_cast<ssize_t>(size), "write file");
    close(fd);
}

static std::string readFile(const std::string &path)
{
    const int fd = open(path.c_str(), O_RDONLY);
    check(fd >= 0, "open snapshot");
    char data[64];
    const ssize_t n = read(fd, data, sizeof(data));
    check(n >= 0, "read snapshot");
    close(fd);
    return std::string(data, static_cast<size_t>(n));
}

int main()
{
    char temp[] = "/tmp/android-compatibility-layer-install-request-test-XXXXXX";
    check(mkdtemp(temp) != nullptr, "create test directory");
    const std::string root = std::string(temp) + "/data";
    const std::string sub = root + "/cache";
    const std::string apk = sub + "/base.apk";
    const std::string outside = std::string(temp) + "/outside.apk";
    check(mkdir(root.c_str(), 0700) == 0 && mkdir(sub.c_str(), 0700) == 0, "create directories");
    writeFile(apk, "first");
    writeFile(outside, "private");

    InstallRequestSnapshot first, second;
    std::string error;
    check(first.capture(root, apk, "app.apk", 100, &error), "capture");
    check(second.capture(root, apk, "app.apk", 100, &error), "parallel snapshot");
    QFile qtSnapshot(QString::fromStdString(first.path()));
    check(qtSnapshot.open(QIODevice::ReadOnly) && qtSnapshot.readAll() == QByteArray("first"),
          "Qt can read snapshot");
    qtSnapshot.close();
    writeFile(apk, "changed");
    check(readFile(first.path()) == "first" && readFile(second.path()) == "first", "snapshot unchanged");
    int fd = open(first.path().c_str(), O_WRONLY);
    check(fd >= 0, "open snapshot for seal check");
    check(write(fd, "x", 1) == -1 && errno == EPERM, "snapshot sealed");
    close(fd);
    check(unlink(apk.c_str()) == 0 && readFile(first.path()) == "first", "source disappears");

    check(!first.capture(root, outside, "app.apk", 100, &error) && !error.empty() && first.path().empty(), "outside root");
    check(!first.capture(root, root + "/../outside.apk", "app.apk", 100, &error), "traversal");
    check(!first.capture(root, root + "2/app.apk", "app.apk", 100, &error), "prefix boundary");
    check(!first.capture(root, root + "/cache//base.apk", "app.apk", 100, &error), "empty component");
    check(symlink(outside.c_str(), apk.c_str()) == 0, "create file symlink");
    check(!first.capture(root, apk, "app.apk", 100, &error), "file symlink");
    check(unlink(apk.c_str()) == 0, "remove symlink");
    check(symlink(temp, (root + "/link").c_str()) == 0, "create directory symlink");
    check(!first.capture(root, root + "/link/outside.apk", "app.apk", 100, &error), "directory symlink");
    check(link(outside.c_str(), apk.c_str()) == 0, "create hard link");
    check(!first.capture(root, apk, "app.apk", 100, &error), "hard link");
    check(unlink(apk.c_str()) == 0, "remove hard link");
    check(mkfifo(apk.c_str(), 0600) == 0, "create fifo");
    check(!first.capture(root, apk, "app.apk", 100, &error), "fifo");
    check(unlink(apk.c_str()) == 0, "remove fifo");
    writeFile(apk, "abcdef");
    check(!first.capture(root, apk, "app.apk", 5, &error), "size limit");
    check(!first.capture(root, apk, "../app.apk", 100, &error), "untrusted name");
    check(!first.capture(root, apk, "app\033.apk", 100, &error), "control characters in name");
    check(!first.capture(root, apk, "bundle.APKS", 100, &error), "bundle name");
    check(!first.capture(root, sub + "/bundle.xapk", "app.apk", 100, &error), "bundle path");
    check(!first.capture(root, sub, "app.apk", 100, &error), "directory rejected");
    check(readFile(second.path()) == "first", "other request unaffected");
    second.clear();
    check(second.path().empty(), "cancel clears snapshot");

    check(unlink(apk.c_str()) == 0 && unlink((root + "/link").c_str()) == 0 &&
          unlink(outside.c_str()) == 0 && rmdir(sub.c_str()) == 0 &&
          rmdir(root.c_str()) == 0 && rmdir(temp) == 0, "cleanup");
    std::puts("Install request snapshot OK");
    return 0;
}
