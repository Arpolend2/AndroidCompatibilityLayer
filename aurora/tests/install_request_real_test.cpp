#include "../install_request_candidate.h"
#include "../apk_store.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 2 && argc != 3) return 2;
    QTemporaryDir directory;
    const QString apk = directory.path() + QStringLiteral("/base.apk");
    if (!directory.isValid() || !QFile::copy(QString::fromLocal8Bit(argv[1]), apk)) return 1;
    InstallRequestCandidate candidate;
    QString error;
    if (!candidate.prepare(directory.path(), apk, QStringLiteral("app.apk"), 512u * 1024 * 1024, &error)) {
        std::fprintf(stderr, "Inspection failed: %s\n", qPrintable(error));
        return 1;
    }
    const QString expected = argc == 3 ? QString::fromLocal8Bit(argv[2]) : QStringLiteral("megler.test.hello");
    if (candidate.package() != expected) return 1;
    if (!QFile::remove(apk)) return 1;
    ApkStore store(directory.path() + QStringLiteral("/store"));
    if (!store.install(candidate.path(), &error)) {
        std::fprintf(stderr, "Installation failed: %s\n", qPrintable(error));
        return 1;
    }
    if (store.apps().size() != 1 || !QFile::exists(store.apkPath(candidate.package()))) return 1;
    if (store.install(candidate.path(), &error)) return 1;
    std::puts("Real APK snapshot installation OK");
    return 0;
}
