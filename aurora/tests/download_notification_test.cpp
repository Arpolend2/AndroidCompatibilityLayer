#include <QDBusConnection>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QTimer>

#include <cstdio>

class NotificationService : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")
public slots:
    uint Notify(const QString &, uint replacesId, const QString &, const QString &summary,
                const QString &body, const QStringList &, const QVariantMap &hints, int)
    {
        QJsonObject record;
        record.insert(QStringLiteral("replaces"), int(replacesId));
        record.insert(QStringLiteral("summary"), summary);
        record.insert(QStringLiteral("body"), body);
        record.insert(QStringLiteral("previewSummary"), hints.value(QStringLiteral("x-nemo-preview-summary")).toString());
        record.insert(QStringLiteral("previewBody"), hints.value(QStringLiteral("x-nemo-preview-body")).toString());
        record.insert(QStringLiteral("hasProgress"), hints.contains(QStringLiteral("x-nemo-progress")));
        record.insert(QStringLiteral("progress"), hints.value(QStringLiteral("x-nemo-progress")).toDouble());
        std::puts(QJsonDocument(record).toJson(QJsonDocument::Compact).constData());
        std::fflush(stdout);
        static uint nextId = 42;
        return replacesId ? replacesId : nextId++;
    }
};

static QJsonObject readRecord(QProcess &service)
{
    while (!service.canReadLine()) {
        if (!service.waitForReadyRead(5000)) return {};
    }
    return QJsonDocument::fromJson(service.readLine()).object();
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    if (argc == 2 && QByteArray(argv[1]) == "--service") {
        NotificationService service;
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (!bus.registerService(QStringLiteral("org.freedesktop.Notifications")) ||
            !bus.registerObject(QStringLiteral("/org/freedesktop/Notifications"), &service,
                                QDBusConnection::ExportAllSlots)) return 1;
        std::puts("ready");
        std::fflush(stdout);
        return app.exec();
    }
    if (argc != 2) return 2;
    QProcess service;
    service.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--service")});
    if (!service.waitForStarted() || !service.waitForReadyRead(5000) ||
        service.readLine().trimmed() != "ready") return 1;

    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QString::fromLocal8Bit(argv[1])));
    QScopedPointer<QObject> notification(component.create());
    if (!notification) {
        for (const QQmlError &error : component.errors())
            std::fprintf(stderr, "%s\n", qPrintable(error.toString()));
        return 1;
    }
    notification->setProperty("title", QStringLiteral("microG"));
    auto publish = [&](const QString &message, int percent, bool complete) {
        return QMetaObject::invokeMethod(notification.data(), "publishUpdate",
                                         Q_ARG(QVariant, message), Q_ARG(QVariant, percent),
                                         Q_ARG(QVariant, complete));
    };
    if (!publish(QStringLiteral("Checking release"), -1, false)) return 1;
    QJsonObject banner = readRecord(service);
    if (banner.value(QStringLiteral("replaces")).toInt(-1) != 0 ||
        !banner.value(QStringLiteral("summary")).toString().isEmpty() ||
        !banner.value(QStringLiteral("body")).toString().isEmpty() ||
        banner.value(QStringLiteral("previewSummary")).toString() != QStringLiteral("microG") ||
        banner.value(QStringLiteral("previewBody")).toString() != QStringLiteral("Checking release") ||
        banner.value(QStringLiteral("hasProgress")).toBool()) return 1;
    QJsonObject first = readRecord(service);
    if (first.value(QStringLiteral("replaces")).toInt(-1) != 0 ||
        !first.value(QStringLiteral("previewSummary")).toString().isEmpty() ||
        !first.value(QStringLiteral("previewBody")).toString().isEmpty() ||
        !first.value(QStringLiteral("hasProgress")).toBool() ||
        first.value(QStringLiteral("progress")).toDouble() != -1) return 1;
    if (!publish(QStringLiteral("Downloading"), 10, false) ||
        !publish(QStringLiteral("Downloading"), 55, false)) return 1;
    QEventLoop wait;
    QTimer::singleShot(1200, &wait, &QEventLoop::quit);
    wait.exec();
    QJsonObject progress = readRecord(service);
    if (progress.value(QStringLiteral("replaces")).toInt() != 43 ||
        progress.value(QStringLiteral("summary")).toString() != QStringLiteral("microG") ||
        !progress.value(QStringLiteral("previewSummary")).toString().isEmpty() ||
        !progress.value(QStringLiteral("previewBody")).toString().isEmpty() ||
        progress.value(QStringLiteral("progress")).toDouble() != 0.55 ||
        !progress.value(QStringLiteral("body")).toString().contains(QStringLiteral("55%"))) return 1;
    if (!publish(QStringLiteral("Installed"), -1, true)) return 1;
    QJsonObject finished = readRecord(service);
    if (finished.value(QStringLiteral("replaces")).toInt() != 43 ||
        !finished.value(QStringLiteral("previewSummary")).toString().isEmpty() ||
        !finished.value(QStringLiteral("previewBody")).toString().isEmpty() ||
        finished.value(QStringLiteral("hasProgress")).toBool() ||
        finished.value(QStringLiteral("body")).toString() != QStringLiteral("Installed")) return 1;
    if (!publish(QStringLiteral("Checking again"), -1, false)) return 1;
    QJsonObject nextBanner = readRecord(service);
    QJsonObject nextProgress = readRecord(service);
    if (nextBanner.value(QStringLiteral("replaces")).toInt(-1) != 0 ||
        nextBanner.value(QStringLiteral("previewBody")).toString() != QStringLiteral("Checking again") ||
        nextProgress.value(QStringLiteral("replaces")).toInt(-1) != 0 ||
        !nextProgress.value(QStringLiteral("previewBody")).toString().isEmpty()) return 1;
    notification.reset();
    QJsonObject interrupted = readRecord(service);
    if (interrupted.value(QStringLiteral("replaces")).toInt() != 45 ||
        interrupted.value(QStringLiteral("hasProgress")).toBool() ||
        !interrupted.value(QStringLiteral("body")).toString().contains(QStringLiteral("interrupted"))) return 1;
    service.terminate();
    service.waitForFinished();
    std::puts("Download notification OK");
    return 0;
}

#include "download_notification_test.moc"
