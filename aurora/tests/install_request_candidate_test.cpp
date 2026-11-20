#include "../install_request_candidate.h"
#include "../install_request_prepare.h"
#include "../../core/src/apk/mg_apk.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <cstring>

static void check(bool ok, const char *description)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        std::exit(1);
    }
}

extern "C" bool mg_apk_read_info(const char *path, mg_apk_info *info)
{
    std::memset(info, 0, sizeof(*info));
    QFile file(QString::fromUtf8(path));
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray contents = file.readAll();
    if (contents == "invalid") return false;
    std::strcpy(info->package, contents == "badpackage" ? "../private" : "org.example.app");
    std::strcpy(info->label, contents == "spoof" ? "Example\342\200\256evil" : "Example");
    info->version_code = 42;
    info->runnable = contents != "unrunnable";
    info->split_required = contents == "base";
    if (contents == "split") std::strcpy(info->split, "config.en");
    if (contents == "types") std::strcpy(info->required_split_types, "base__abi");
    return true;
}

static void writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "open input");
    check(file.write(contents) == contents.size(), "write input");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    check(directory.isValid(), "temporary root");
    const QString apk = directory.path() + QStringLiteral("/base.apk");
    InstallRequestCandidate candidate;
    QString error;
    writeFile(apk, "whole");
    check(candidate.prepare(directory.path(), apk, QStringLiteral("app.apk"), 1024, &error, true), "prepare");
    check(candidate.package() == QLatin1String("org.example.app") &&
          candidate.label() == QLatin1String("Example") && candidate.version() == 42, "metadata");
    writeFile(apk, "changed");
    QFile snapshot(candidate.path());
    check(snapshot.open(QIODevice::ReadOnly) && snapshot.readAll() == "whole", "reviewed snapshot retained");
    check(candidate.fingerprint() == QCryptographicHash::hash(QByteArray("whole"), QCryptographicHash::Sha256).toHex(),
          "fingerprint identifies the retained snapshot");
    snapshot.close();

    for (const QByteArray &input : {QByteArray("invalid"), QByteArray("badpackage"), QByteArray("unrunnable"),
                                    QByteArray("base"), QByteArray("split"), QByteArray("types")}) {
        writeFile(apk, input);
        check(!candidate.prepare(directory.path(), apk, QStringLiteral("app.apk"), 1024, &error) &&
              !error.isEmpty() && candidate.path().isEmpty() && candidate.package().isEmpty(),
              "invalid candidate rejected and cleared");
    }
    writeFile(apk, "spoof");
    check(candidate.prepare(directory.path(), apk, QStringLiteral("app.apk"), 1024, &error), "prepare spoof label");
    check(candidate.label() == candidate.package(), "unsafe label replaced with package");
    const QString prefix = QStringLiteral("request_0123456789abcdef0123456789abcdef_42_");
    check(candidate.prepare(directory.path(), apk, prefix + QStringLiteral("org.example.app.apk"), 1024, &error),
          "matching session package accepted");
    check(!candidate.prepare(directory.path(), apk, prefix + QStringLiteral("org.example.other.apk"), 1024, &error) &&
          candidate.path().isEmpty(), "different snapshot package rejected");
    check(!candidate.prepare(directory.path(), apk, QStringLiteral("request_0123456789abcdef0123456789abcdef_41_org.example.app.apk"), 1024, &error),
          "different snapshot version rejected");
    candidate.clear();
    check(candidate.path().isEmpty(), "clear releases snapshot");

    InstallRequestPrepare prepare;
    QEventLoop loop;
    bool ready = false;
    QObject::connect(&prepare, &InstallRequestPrepare::ready, &loop, [&](quint64 id) {
        check(id == 2 && prepare.candidate(id), "only live request becomes ready");
        ready = true;
        prepare.cancel(id);
        check(!prepare.candidate(id) && !prepare.busy(), "cancel clears prepared snapshot");
        loop.quit();
    });
    QObject::connect(&prepare, &InstallRequestPrepare::failed, &loop,
                     [&](quint64, const QString &) { check(false, "preparation succeeds"); });
    QObject::connect(&prepare, &InstallRequestPrepare::available, &loop, [&]() {
        if (!ready) check(prepare.start(2, directory.path(), apk, QStringLiteral("app.apk"), 1024),
                          "next request starts when canceled worker is reaped");
    });
    check(prepare.start(1, directory.path(), apk, QStringLiteral("app.apk"), 1024), "start worker");
    check(!prepare.start(2, directory.path(), apk, QStringLiteral("app.apk"), 1024), "worker queue bounded");
    prepare.cancel(1);
    check(!prepare.candidate(1) && prepare.busy(), "cancel invalidates immediately while worker finishes");
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    loop.exec();
    check(ready, "worker finished");
    std::puts("Install request candidate OK");
    return 0;
}
