#include "../install_request_commit.h"
#include "../app_storage_lock.h"

#include <QDir>
#include <QFile>
#include <QLockFile>
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

int main()
{
    QTemporaryDir directory;
    check(directory.isValid(), "temporary directory");
    const QString source = directory.path() + QStringLiteral("/source.apk");
    QFile input(source);
    check(input.open(QIODevice::WriteOnly) && input.write("approved") == 8, "source created");
    input.close();
    const QString root = directory.path() + QStringLiteral("/store");
    InstallRequestCommit stage(source, root, QStringLiteral("org.example.app"));
    stage.start();
    stage.wait();
    if (!stage.error().isEmpty()) std::fprintf(stderr, "%s\n", qPrintable(stage.error()));
    check(stage.error().isEmpty(), "staging succeeds");
    check(input.open(QIODevice::WriteOnly | QIODevice::Truncate) && input.write("changed") == 7, "source changed");
    input.close();
    QString error;
    const bool committed = stage.commit(&error);
    if (!committed) std::fprintf(stderr, "%s\n", qPrintable(error));
    check(committed, "commit succeeds");
    QFile stored(root + QStringLiteral("/org.example.app/base.apk"));
    check(stored.open(QIODevice::ReadOnly) && stored.readAll() == "approved", "commits staged contents");
    stored.close();
    check(!stage.commit(&error), "updates rejected");
    const QString data = root + QStringLiteral("/org.example.app/data");
    check(QDir().mkpath(data), "create app data");
    QFile saved(data + QStringLiteral("/saved"));
    check(saved.open(QIODevice::WriteOnly) && saved.write("data") == 4, "write app data");
    saved.close();
    const QString cache = root + QStringLiteral("/org.example.app/standalone-rpm");
    const QByteArray id("ru.androidcompatibilitylayer.Android0123456789abcdef01234567");
    check(QDir().mkpath(cache), "create RPM cache");
    QFile mapping(cache + QStringLiteral("/.last-created-app-id"));
    check(mapping.open(QIODevice::WriteOnly) && mapping.write(id) == id.size(), "write RPM mapping");
    mapping.close();
    const QString armRpm = cache + QLatin1Char('/') + QString::fromLatin1(id) +
                           QStringLiteral("-1.0.0-1.armv7hl.rpm");
    QFile armFile(armRpm);
    check(armFile.open(QIODevice::WriteOnly), "create ARMv7 cached RPM");
    armFile.close();
    InstallRequestCommit update(source, root, QStringLiteral("org.example.app"), true);
    update.start();
    update.wait();
    check(update.error().isEmpty(), "update staging succeeds");
    QLockFile lock(appStorageLockPath(root, QStringLiteral("org.example.app")));
    lock.setStaleLockTime(0);
    check(lock.tryLock(0), "lock running app");
    check(!update.commit(&error), "running app update rejected");
    lock.unlock();
    check(update.commit(&error), "update succeeds");
    check(!QFile::exists(armRpm), "update removes stale ARMv7 RPM");
    check(stored.open(QIODevice::ReadOnly) && stored.readAll() == "changed", "updates staged contents");
    stored.close();
    check(saved.open(QIODevice::ReadOnly) && saved.readAll() == "data", "update preserves app data");
    saved.close();
    const QString linked = root + QStringLiteral("/org.example.link");
    check(QDir().mkpath(linked), "create linked package");
    check(QFile::link(stored.fileName(), linked + QStringLiteral("/base.apk")), "create linked APK");
    InstallRequestCommit symlink(source, root, QStringLiteral("org.example.link"), true);
    symlink.start();
    symlink.wait();
    check(!symlink.commit(&error), "refuse to update linked APK");
    check(stored.open(QIODevice::ReadOnly) && stored.readAll() == "changed", "linked target remains unchanged");
    stored.close();
    InstallRequestCommit canceled(source, root, QStringLiteral("org.example.other"));
    canceled.start();
    canceled.wait();
    canceled.cancel();
    check(!canceled.commit(&error) && !QFile::exists(root + QStringLiteral("/org.example.other/base.apk")),
          "canceled request does not commit");
    std::puts("Install request commit OK");
    return 0;
}
