#include <QDBusConnection>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QThread>
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
        record.insert(QStringLiteral("transient"), hints.value(QStringLiteral("transient")).toBool());
        std::puts(QJsonDocument(record).toJson(QJsonDocument::Compact).constData());
        std::fflush(stdout);
        if (summary.contains(QStringLiteral("org.example.delayed"))) QThread::msleep(350);
        static uint nextId = 42;
        return replacesId ? replacesId : nextId++;
    }
    void CloseNotification(uint id)
    {
        QJsonObject record;
        record.insert(QStringLiteral("closed"), int(id));
        std::puts(QJsonDocument(record).toJson(QJsonDocument::Compact).constData());
        std::fflush(stdout);
    }
};

static void settle()
{
    QEventLoop wait;
    QTimer::singleShot(150, &wait, &QEventLoop::quit);
    wait.exec();
}

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
    if (argc != 2 && !(argc == 3 && QByteArray(argv[2]) == "--rpm")) return 2;
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
    if (argc == 3) {
        notification->setProperty("appPackage", QStringLiteral("org.example.app"));
        notification->setProperty("status", QStringLiteral("Inspecting"));
        notification->setProperty("detailsVisible", true);
        notification->setProperty("busy", true);
        settle();
        if (service.canReadLine()) return 1;
        notification->setProperty("detailsVisible", false);
        settle();
        QJsonObject banner = readRecord(service);
        QJsonObject first = readRecord(service);
        if (banner.value(QStringLiteral("replaces")).toInt(-1) != 0 ||
            !banner.value(QStringLiteral("summary")).toString().isEmpty() ||
            !banner.value(QStringLiteral("body")).toString().isEmpty() ||
            banner.value(QStringLiteral("previewSummary")).toString() != QStringLiteral("Creating RPM for org.example.app") ||
            banner.value(QStringLiteral("previewBody")).toString() != QStringLiteral("Inspecting") ||
            banner.value(QStringLiteral("hasProgress")).toBool() ||
            !banner.value(QStringLiteral("transient")).toBool() ||
            first.value(QStringLiteral("replaces")).toInt(-1) != 0 ||
            first.value(QStringLiteral("summary")).toString() != QStringLiteral("Creating RPM for org.example.app") ||
            first.value(QStringLiteral("body")).toString() != QStringLiteral("Inspecting") ||
            !first.value(QStringLiteral("previewBody")).toString().isEmpty() ||
            !first.value(QStringLiteral("hasProgress")).toBool() ||
            first.value(QStringLiteral("progress")).toDouble() != -1) return 1;
        notification->setProperty("status", QStringLiteral("Building"));
        settle();
        QJsonObject buildingBanner = readRecord(service);
        QJsonObject building = readRecord(service);
        if (buildingBanner.value(QStringLiteral("previewBody")).toString() != QStringLiteral("Building") ||
            building.value(QStringLiteral("replaces")).toInt() != 43 ||
            building.value(QStringLiteral("body")).toString() != QStringLiteral("Building") ||
            !building.value(QStringLiteral("previewBody")).toString().isEmpty() ||
            building.value(QStringLiteral("progress")).toDouble() != -1) return 1;
        notification->setProperty("status", QStringLiteral("Signing"));
        settle();
        QJsonObject signingBanner = readRecord(service);
        QJsonObject signing = readRecord(service);
        if (signingBanner.value(QStringLiteral("previewBody")).toString() != QStringLiteral("Signing") ||
            signing.value(QStringLiteral("replaces")).toInt() != 43 ||
            signing.value(QStringLiteral("body")).toString() != QStringLiteral("Signing")) return 1;
        notification->setProperty("status", QStringLiteral("Saving"));
        settle();
        QJsonObject savingBanner = readRecord(service);
        QJsonObject saving = readRecord(service);
        if (savingBanner.value(QStringLiteral("previewBody")).toString() != QStringLiteral("Saving") ||
            saving.value(QStringLiteral("replaces")).toInt() != 43 ||
            saving.value(QStringLiteral("body")).toString() != QStringLiteral("Saving")) return 1;
        notification->setProperty("detailsVisible", true);
        settle();
        if (readRecord(service).value(QStringLiteral("closed")).toInt() != 43) return 1;
        notification->setProperty("detailsVisible", false);
        settle();
        QJsonObject returnedBanner = readRecord(service);
        QJsonObject returned = readRecord(service);
        if (returnedBanner.value(QStringLiteral("previewBody")).toString() != QStringLiteral("Saving") ||
            returned.value(QStringLiteral("replaces")).toInt(-1) != 0 ||
            returned.value(QStringLiteral("body")).toString() != QStringLiteral("Saving")) return 1;
        notification->setProperty("busy", false);
        notification->setProperty("status", QStringLiteral("Failed"));
        settle();
        if (readRecord(service).value(QStringLiteral("closed")).toInt() != 48) return 1;
        notification->setProperty("appPackage", QStringLiteral("org.example.next"));
        notification->setProperty("status", QStringLiteral("Inspecting again"));
        notification->setProperty("busy", true);
        settle();
        QJsonObject nextBanner = readRecord(service);
        QJsonObject next = readRecord(service);
        if (nextBanner.value(QStringLiteral("previewBody")).toString() != QStringLiteral("Inspecting again") ||
            next.value(QStringLiteral("replaces")).toInt(-1) != 0 ||
            next.value(QStringLiteral("summary")).toString() != QStringLiteral("Creating RPM for org.example.next")) return 1;
        notification.reset();
        settle();
        if (readRecord(service).value(QStringLiteral("closed")).toInt() != 50) return 1;
        notification.reset(component.create());
        if (!notification) return 1;
        notification->setProperty("appPackage", QStringLiteral("org.example.delayed"));
        notification->setProperty("status", QStringLiteral("Inspecting"));
        notification->setProperty("busy", true);
        QEventLoop briefWait;
        QTimer::singleShot(50, &briefWait, &QEventLoop::quit);
        briefWait.exec();
        notification->setProperty("busy", false);
        QEventLoop delayedWait;
        QTimer::singleShot(600, &delayedWait, &QEventLoop::quit);
        delayedWait.exec();
        if (readRecord(service).value(QStringLiteral("previewBody")).toString() != QStringLiteral("Inspecting") ||
            readRecord(service).value(QStringLiteral("replaces")).toInt(-1) != 0 ||
            readRecord(service).value(QStringLiteral("closed")).toInt() != 52) return 1;
        notification.reset();
        service.terminate();
        service.waitForFinished();
        std::puts("Standalone RPM notification OK");
        return 0;
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
