#include "../standalone_apk_export.h"
#include "../external_apk_install.h"
#include "../../core/src/apk/mg_apk.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include <cstdio>
#include <cstring>

extern "C" bool mg_apk_read_info(const char *path, mg_apk_info *info)
{
    QFile file(QString::fromUtf8(path));
    if (!file.open(QIODevice::ReadOnly) || file.readAll() != "test APK") return false;
    std::memset(info, 0, sizeof(*info));
    std::strcpy(info->package, "org.example.app");
    std::strcpy(info->label, "Example");
    info->version_code = 42;
    info->runnable = true;
    return true;
}

class Handler : public QObject
{
    Q_OBJECT
public:
    QString path;
public slots:
    void open(const QUrl &url) { path = url.toLocalFile(); emit opened(path); }
signals:
    void opened(const QString &path);
};

int main(int argc, char **argv)
{
    QTemporaryDir root;
    if (!root.isValid()) return 1;
    qputenv("HOME", QFile::encodeName(root.path()));
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(root.path()));
    QGuiApplication app(argc, argv);
    const QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (downloads.isEmpty() || !QDir().mkpath(downloads)) return 1;
    Handler handler;
    QDesktopServices::setUrlHandler(QStringLiteral("file"), &handler, "open");
    const QString store = QDir(root.path()).filePath(QStringLiteral("library"));
    ExternalApkInstall manager(store);
    bool imported = false, importFailed = false;
    QObject::connect(&handler, &Handler::opened, &app, [&](const QString &path) {
        QString error;
        if (!manager.start(path, &error)) importFailed = true;
    });
    QObject::connect(&manager, &ExternalApkInstall::confirmationRequested, &app,
                     [&](const QString &, const QString &, quint32) { manager.decision(true); });
    QObject::connect(&manager, &ExternalApkInstall::finished, &app, [&](bool ok, const QString &) {
        imported = ok;
        importFailed = !ok;
    });
    megler_system_host other = {};
    StandaloneApkExport exporter(root.path(), other);
    if (!exporter.error().isEmpty()) return 1;
    const megler_system_host callbacks = exporter.host();
    const QByteArray name("request_0123456789abcdef0123456789abcdef.apk");
    if (!callbacks.install_apk || callbacks.install_apk(callbacks.user, "/outside.apk", "../bad.apk"))
        return 1;
    if (!callbacks.install_apk(callbacks.user, "/outside.apk", name.constData()))
        return 1;
    const QString result = QDir(exporter.appsDir()).filePath(QStringLiteral("installs/") +
                                                            QString::fromLatin1(name) + QStringLiteral(".txt"));
    const QString source = QDir(root.path()).filePath(QStringLiteral("source.apk"));
    QFile apk(source);
    if (!apk.open(QIODevice::WriteOnly) || apk.write("test APK") != 8) return 1;
    apk.close();
    int stage = 0;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &app, [&]() {
        QFile file(result);
        if (!file.open(QIODevice::ReadOnly)) return;
        const QByteArray status = file.readAll();
        file.close();
        if (!status.contains("\tfailed\t")) { app.exit(1); return; }
        if (stage == 0) {
            if (!QFile::remove(result) ||
                !callbacks.install_apk(callbacks.user, QFile::encodeName(source).constData(), name.constData())) {
                app.exit(1);
                return;
            }
            stage = 1;
        } else {
            if (importFailed) { app.exit(1); return; }
            if (!imported) return;
            QFile exported(handler.path);
            QFile saved(QDir(store).filePath(QStringLiteral("org.example.app/base.apk")));
            if (handler.path.isEmpty() || !exported.open(QIODevice::ReadOnly) ||
                exported.readAll() != "test APK" || !saved.open(QIODevice::ReadOnly) ||
                saved.readAll() != "test APK" || !status.contains("cannot be reported")) app.exit(1);
            else app.quit();
        }
    });
    poll.start(10);
    QTimer::singleShot(5000, &app, [&]() { app.exit(1); });
    const int resultCode = app.exec();
    if (!resultCode) std::puts("Standalone APK export OK");
    return resultCode;
}

#include "standalone_apk_export_test.moc"
