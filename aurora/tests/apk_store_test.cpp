#include "../apk_store.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QTemporaryDir>

#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 2) return 2;
    QTemporaryDir directory;
    if (!directory.isValid()) return 1;
    ApkStore store(directory.path());
    QString message;
    if (!store.install(QString::fromLocal8Bit(argv[1]), &message)) {
        std::fprintf(stderr, "Install failed: %s\n", qPrintable(message));
        return 1;
    }
    const QVariantList apps = ApkStore(directory.path()).apps();
    if (apps.size() != 1) return 1;
    const QString package = apps.first().toMap().value(QStringLiteral("package")).toString();
    if (package != QStringLiteral("megler.test.hello") || !QFile::exists(store.apkPath(package))) return 1;
    if (!apps.first().toMap().value(QStringLiteral("icon")).toString().startsWith(QStringLiteral("data:image/png;base64,"))) return 1;
    if (apps.first().toMap().value(QStringLiteral("launchCount")).toULongLong() != 0 ||
        !store.recordLaunch(package, &message) || !store.recordLaunch(package, &message) ||
        ApkStore(directory.path()).apps().first().toMap().value(QStringLiteral("launchCount")).toULongLong() != 2 ||
        store.recordLaunch(QStringLiteral("../escape"), &message)) return 1;
    QFile usage(QDir(directory.path()).filePath(package + QStringLiteral("/.launch-count")));
    if (!usage.open(QIODevice::WriteOnly | QIODevice::Truncate) || usage.write("invalid") != 7) return 1;
    usage.close();
    if (store.recordLaunch(package, &message) || message.isEmpty() ||
        !usage.open(QIODevice::ReadOnly) || usage.readAll() != "invalid") return 1;
    usage.close();
    if (!store.apkPath(QStringLiteral("../escape")).isEmpty()) return 1;
    if (store.install(QString::fromLocal8Bit(argv[1]), &message)) return 1;
    if (ApkStore(directory.path()).apps().size() != 1 || !QFile::exists(store.apkPath(package))) return 1;
    const QString bad = QDir(directory.path()).filePath(QStringLiteral("invalid.apk"));
    QFile invalid(bad);
    if (!invalid.open(QIODevice::WriteOnly) || invalid.write("not an APK") != 10) return 1;
    invalid.close();
    if (store.install(bad, &message) || ApkStore(directory.path()).apps().size() != 1) return 1;
    if (store.remove(QStringLiteral("../escape"), &message) || !QFile::exists(store.apkPath(package))) return 1;
    const QString data = store.dataPath(package);
    if (!QDir().mkpath(data)) return 1;
    QFile saved(QDir(data).filePath(QStringLiteral("saved")));
    if (!saved.open(QIODevice::WriteOnly) || saved.write("data") != 4) return 1;
    saved.close();
    QLockFile lock(store.lockPath(package));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0) || store.remove(package, &message) || !QFile::exists(saved.fileName())) return 1;
    lock.unlock();
    if (!store.remove(package, &message) || QFile::exists(store.apkPath(package)) ||
        QFile::exists(saved.fileName()) || !store.apps().isEmpty()) return 1;
    const QString other = QDir(directory.path()).filePath(QStringLiteral("other"));
    if (!QDir().mkpath(other)) return 1;
    QFile otherApk(QDir(other).filePath(QStringLiteral("base.apk")));
    if (!otherApk.open(QIODevice::WriteOnly) || otherApk.write("other") != 5) return 1;
    otherApk.close();
    const QString link = QDir(directory.path()).filePath(QStringLiteral("org.example.link"));
    if (!QFile::link(other, link) || store.remove(QStringLiteral("org.example.link"), &message) ||
        !QFile::exists(otherApk.fileName())) return 1;
    if (!store.install(QString::fromLocal8Bit(argv[1]), &message)) return 1;
    std::puts("APK store OK");
    return 0;
}
