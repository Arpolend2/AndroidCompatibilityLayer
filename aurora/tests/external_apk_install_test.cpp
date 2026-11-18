#include "../external_apk_install.h"
#include "../../core/src/apk/mg_apk.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
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
    QFile file(QString::fromUtf8(path));
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray contents = file.readAll();
    if (contents == "invalid") return false;
    std::memset(info, 0, sizeof(*info));
    std::strcpy(info->package, "org.example.app");
    std::strcpy(info->label, "Example");
    info->version_code = 42;
    info->runnable = true;
    info->split_required = contents == "split";
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
    const QString store = directory.path() + QStringLiteral("/store");
    ExternalApkInstall install(store);
    QString mode;
    bool prompted = false, completed = false, installed = false;
    QString message;
    QEventLoop loop;
    QObject::connect(&install, &ExternalApkInstall::confirmationRequested, &loop,
                     [&](const QString &label, const QString &package, quint32 version) {
        prompted = true;
        check(label == QLatin1String("Example") && package == QLatin1String("org.example.app") &&
              version == 42, "reviewed metadata");
        check(install.isUpdate() == (mode == QLatin1String("update-approved")), "update confirmation state");
        check(QFile::exists(store + QStringLiteral("/org.example.app/base.apk")) ==
              (mode == QLatin1String("update-approved")), "existing APK is retained until approval");
        if (mode == QLatin1String("accept") || mode == QLatin1String("update-approved"))
            writeFile(apk, "changed");
        if (mode == QLatin1String("unavailable")) install.unavailable();
        else install.decision(mode == QLatin1String("accept") || mode == QLatin1String("update-approved"));
    });
    QObject::connect(&install, &ExternalApkInstall::finished, &loop,
                     [&](bool ok, const QString &text) {
        completed = true;
        installed = ok;
        message = text;
        loop.quit();
    });

    QString missingExpected;
    check(!install.start(apk, &missingExpected, QString(), true) && !missingExpected.isEmpty(),
          "update requires expected package");
    for (const QString &next : {QStringLiteral("decline"), QStringLiteral("unavailable"),
                                QStringLiteral("invalid"), QStringLiteral("split"),
                                QStringLiteral("symlink"), QStringLiteral("wrong-package"), QStringLiteral("accept"),
                                QStringLiteral("update"), QStringLiteral("update-approved")}) {
        mode = next;
        prompted = completed = installed = false;
        message.clear();
        writeFile(apk, mode == QLatin1String("update-approved") ? "update payload" :
                       mode == QLatin1String("split") ? "split" :
                       mode == QLatin1String("invalid") ? "invalid" : "original");
        const QString source = mode == QLatin1String("symlink")
            ? directory.path() + QStringLiteral("/link.apk") : apk;
        if (mode == QLatin1String("symlink")) check(QFile::link(apk, source), "create symlink");
        QString error;
        check(install.start(source, &error, mode == QLatin1String("wrong-package")
                            ? QStringLiteral("org.example.other")
                            : mode == QLatin1String("update-approved") ? QStringLiteral("org.example.app") : QString(),
                            mode == QLatin1String("update-approved")), "start request");
        check(!install.start(apk, &error) && !error.isEmpty(), "concurrent request refused");
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(5000);
        loop.exec();
        check(completed && !message.isEmpty(), "request completed with explicit result");
        check(prompted == (mode == QLatin1String("decline") || mode == QLatin1String("unavailable") ||
                           mode == QLatin1String("accept") || mode == QLatin1String("update-approved")),
              "only compatible APK is prompted");
        check(installed == (mode == QLatin1String("accept") || mode == QLatin1String("update-approved")),
              "only approval commits");
        QFile result(store + QStringLiteral("/org.example.app/base.apk"));
        if (mode == QLatin1String("accept") || mode == QLatin1String("update") ||
            mode == QLatin1String("update-approved")) {
            check(result.open(QIODevice::ReadOnly) &&
                  result.readAll() == (mode == QLatin1String("update-approved") ? "update payload" : "original"),
                  "commit contains reviewed snapshot");
        } else {
            check(!result.exists(), "no unapproved installation");
        }
    }
    std::puts("External APK installation OK");
    return 0;
}
