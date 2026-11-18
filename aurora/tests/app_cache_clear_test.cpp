#include "../app_cache_clear.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstdio>
#include <cstdlib>

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

int main()
{
    QTemporaryDir temporary;
    check(temporary.isValid(), "temporary root");
    const QString appData = temporary.path() + QStringLiteral("/apps/org.example.demo/data");
    const QString standaloneData = temporary.path() + QStringLiteral("/standalone/apps/org.example.demo/data");
    check(QDir().mkpath(appData + QStringLiteral("/cache")) &&
          QDir().mkpath(standaloneData), "create app data directories");
    writeFile(appData + QStringLiteral("/cache/keep.txt"), "preserve");

    QString error;
    check(AppCacheClear::request(appData, QStringLiteral("org.example.demo"), &error),
          "queue valid cache request");
    check(QFile::exists(appData + QStringLiteral("/clear-cache/org.example.demo")),
          "write core-compatible request marker");
    QFile preserved(appData + QStringLiteral("/cache/keep.txt"));
    check(preserved.open(QIODevice::ReadOnly) && preserved.readAll() == "preserve",
          "request does not delete cache files");

    check(AppCacheClear::request(standaloneData, QStringLiteral("org.example.demo"), &error),
          "queue standalone cache request");
    check(QFile::exists(standaloneData + QStringLiteral("/clear-cache/org.example.demo")) &&
          !QFile::exists(standaloneData + QStringLiteral("/clear-cache/other.app")),
          "standalone request stays in its own data root");
    check(!AppCacheClear::request(appData, QStringLiteral("../outside"), &error) && !error.isEmpty(),
          "reject package path traversal");
    check(!AppCacheClear::request(temporary.path(), QStringLiteral("org.example.demo"), &error),
          "reject a non-app data directory");
    std::puts("Aurora app cache clear requests OK");
    return 0;
}
