#include "../microg_install.h"
#include "../../core/src/apk/mg_apk.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdio>
#include <cstring>

#ifndef ANDROID_COMPATIBILITY_LAYER_MICROG_REAL
extern "C" bool mg_apk_read_info(const char *path, mg_apk_info *info)
{
    QFile file(QString::fromUtf8(path));
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray prefix = file.read(5);
    if (prefix == "bad") return false;
    std::memset(info, 0, sizeof(*info));
    std::strcpy(info->package, prefix == "wrong" ? "org.example.other" : "com.google.android.gms");
    return true;
}

extern "C" bool mg_apk_is_microg(const char *) { return true; }
#endif

static void check(bool result, const char *what)
{
    if (!result) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QByteArray json = R"({"tag_name":"v1","assets":[{"name":"com.google.android.gms-123.apk","size":12,"digest":"sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","browser_download_url":"https://github.com/microg/GmsCore/releases/download/v1/com.google.android.gms-123.apk"}]})";
    MicrogRelease release;
    QString error;
    check(microgRelease(json, &release, &error) && release.size == 12 && release.sha256.size() == 32,
          "official metadata");
    check(!microgRelease(QByteArray(json).replace("github.com/microg", "evil.example/microg"), &release, &error),
          "untrusted release host");
    check(!microgRelease(QByteArray(json).replace("sha256:", "sha1:"), &release, &error), "missing digest");
    check(!microgRelease(QByteArray(json).replace("\"size\":12", "\"size\":536870913"), &release, &error),
          "oversized release");
    check(!microgDownloadUrlAllowed(QUrl(QStringLiteral("https://github.com.evil.example/a"))) &&
          !microgDownloadUrlAllowed(QUrl(QStringLiteral("http://github.com/a"))) &&
          !microgDownloadUrlAllowed(QUrl(QStringLiteral("https://x@github.com/a"))),
          "redirect host validation");

    QTemporaryDir directory;
    check(directory.isValid(), "temporary directory");
    QCoreApplication::setOrganizationName(QStringLiteral("AndroidCompatibilityLayerTest"));
    QCoreApplication::setApplicationName(QStringLiteral("MicrogInstall"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    const QString target = directory.path() + QStringLiteral("/services/GmsCore.apk");
    MicrogInstall installer(target);
    check(!installer.start(QString(), []() { return false; }, &error) && !error.isEmpty(),
          "running apps block installation");
    if (argc == 1) {
        const QString source = directory.path() + QStringLiteral("/local.apk");
        QFile local(source);
        check(local.open(QIODevice::WriteOnly) && local.write("local") == 5, "write local APK");
        local.close();
        QEventLoop loop;
        bool success = false;
        QObject::connect(&installer, &MicrogInstall::finished, &loop, [&](bool ok, const QString &) {
            success = ok;
            loop.quit();
        });
        check(installer.start(source, []() { return true; }, &error), "start offline installation");
        loop.exec();
        check(success && QFile::exists(target), "offline local installation succeeds");
        const QString data = directory.path() + QStringLiteral("/services/data/saved");
        check(QDir().mkpath(QFileInfo(data).absolutePath()), "create application data");
        QFile saved(data);
        check(saved.open(QIODevice::WriteOnly) && saved.write("data") == 4, "write application data");
        saved.close();
        check(local.open(QIODevice::WriteOnly | QIODevice::Truncate) && local.write("wrong") == 5,
              "write wrong package");
        local.close();
        check(installer.start(source, []() { return true; }, &error), "start wrong-package update");
        loop.exec();
        check(!success && QFileInfo(target).size() == 5 && QFile::exists(data),
              "wrong package preserves installation and data");
        check(local.open(QIODevice::WriteOnly | QIODevice::Truncate) && local.write("update") == 6,
              "write local update");
        local.close();
        check(installer.start(source, []() { return true; }, &error), "start offline update");
        loop.exec();
        check(success && QFileInfo(target).size() == 6 && QFile::exists(data),
              "offline update preserves application data");
    }
    if (argc == 2) {
        QEventLoop loop;
        bool success = false;
        bool expectedFailure = false;
        QObject::connect(&installer, &MicrogInstall::finished, &loop, [&](bool ok, const QString &message) {
            success = ok;
            if (!ok && !expectedFailure) std::fprintf(stderr, "%s\n", qPrintable(message));
            loop.quit();
        });
        const QString argument = QString::fromLocal8Bit(argv[1]);
        const QString source = argument == QLatin1String("--download") ? QString() : argument;
        check(installer.start(source, []() { return true; }, &error), "start update");
        loop.exec();
        check(success && QFile::exists(target), "verified local update");
        QFile installed(target);
        check(installed.open(QIODevice::ReadOnly), "read installed APK");
        const qint64 size = installed.size();
        installed.close();
        QFile replacement(directory.path() + QStringLiteral("/bad.apk"));
        check(replacement.open(QIODevice::WriteOnly) && replacement.write("bad") == 3, "write invalid update");
        replacement.close();
        success = true;
        expectedFailure = true;
        check(installer.start(replacement.fileName(), []() { return true; }, &error), "start invalid update");
        loop.exec();
        check(!success && QFileInfo(target).size() == size, "invalid update preserves existing APK");
        const QString data = directory.path() + QStringLiteral("/services/data");
        check(QDir().mkpath(data), "create existing data");
        QFile saved(data + QStringLiteral("/saved"));
        check(saved.open(QIODevice::WriteOnly) && saved.write("data") == 4, "write existing data");
        saved.close();
        expectedFailure = false;
        check(installer.start(source, []() { return true; }, &error), "start verified update");
        loop.exec();
        check(success && QFile::exists(saved.fileName()), "verified update preserves data");
        check(installed.open(QIODevice::Append) && installed.write("x") == 1, "tamper with existing APK");
        installed.close();
        expectedFailure = true;
        check(installer.start(source, []() { return true; }, &error), "start update of untrusted APK");
        loop.exec();
        check(!success && QFileInfo(target).size() == size + 1 && QFile::exists(saved.fileName()),
              "untrusted installed APK is not replaced");
    }
    std::puts("microG installer OK");
    return 0;
}
