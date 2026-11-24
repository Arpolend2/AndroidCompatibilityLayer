#include "../notification_host.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QThread>
#include <QTimer>

#include <cstdio>

class NotificationService : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")
public slots:
    uint Notify(const QString &app, uint replaces, const QString &, const QString &title,
                const QString &body, const QStringList &, const QVariantMap &hints, int timeout)
    {
        if (m_replaceOnNotify) {
            m_replaceOnNotify = false;
            QDBusConnection bus = QDBusConnection::sessionBus();
            if (!bus.unregisterService(QStringLiteral("org.freedesktop.Notifications"))) {
                sendErrorReply(QStringLiteral("org.freedesktop.DBus.Error.Failed"),
                               QStringLiteral("test service could not release its name"));
                return 0;
            }
            sendErrorReply(QStringLiteral("org.freedesktop.DBus.Error.ServiceUnknown"),
                           QStringLiteral("test service owner changed during Notify"));
            return 0;
        }
        QJsonObject record;
        record.insert(QStringLiteral("app"), app);
        record.insert(QStringLiteral("replaces"), int(replaces));
        record.insert(QStringLiteral("title"), title);
        record.insert(QStringLiteral("body"), body);
        record.insert(QStringLiteral("previewSpecified"), hints.contains(QStringLiteral("x-nemo-preview-summary")) &&
                      hints.contains(QStringLiteral("x-nemo-preview-body")));
        record.insert(QStringLiteral("previewSummary"), hints.value(QStringLiteral("x-nemo-preview-summary")).toString());
        record.insert(QStringLiteral("previewBody"), hints.value(QStringLiteral("x-nemo-preview-body")).toString());
        record.insert(QStringLiteral("suppressSound"), hints.value(QStringLiteral("suppress-sound")).toBool());
        record.insert(QStringLiteral("originPackage"), hints.value(QStringLiteral("x-nemo-origin-package")).toString());
        record.insert(QStringLiteral("hasUrgency"), hints.contains(QStringLiteral("urgency")));
        record.insert(QStringLiteral("urgency"), hints.value(QStringLiteral("urgency")).toInt());
        record.insert(QStringLiteral("urgencyIsInt"), hints.value(QStringLiteral("urgency")).userType() == QMetaType::Int);
        record.insert(QStringLiteral("hasProgress"), hints.contains(QStringLiteral("x-nemo-progress")));
        record.insert(QStringLiteral("progress"), hints.value(QStringLiteral("x-nemo-progress")).toDouble());
        record.insert(QStringLiteral("timeout"), timeout);
        std::puts(QJsonDocument(record).toJson(QJsonDocument::Compact).constData());
        std::fflush(stdout);
        return replaces ? replaces : ++m_next;
    }

    void CloseNotification(uint id)
    {
        QJsonObject record;
        record.insert(QStringLiteral("closed"), int(id));
        std::puts(QJsonDocument(record).toJson(QJsonDocument::Compact).constData());
        std::fflush(stdout);
        emit NotificationClosed(id, 3);
    }

    void Dismiss(uint id) { emit NotificationClosed(id, 2); }

signals:
    void NotificationClosed(uint id, uint reason);

private:
    uint m_next = 40;
    bool m_replaceOnNotify = false;

public:
    explicit NotificationService(bool replaceOnNotify = false) : m_replaceOnNotify(replaceOnNotify) {}
};

static bool startService(QProcess &process, const QString &mode = QStringLiteral("--service"))
{
    process.start(QCoreApplication::applicationFilePath(), {mode});
    return process.waitForStarted() && process.waitForReadyRead(5000) &&
           process.readLine().trimmed() == "ready";
}

static QJsonObject record(QProcess &process)
{
    while (!process.canReadLine())
        if (!process.waitForReadyRead(5000)) return {};
    return QJsonDocument::fromJson(process.readLine()).object();
}

static bool require(bool ok, const char *reason)
{
    if (!ok) std::fprintf(stderr, "%s\n", reason);
    return ok;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc == 2 && QByteArray(argv[1]) == "--service") {
        NotificationService service;
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (!bus.registerService(QStringLiteral("org.freedesktop.Notifications")) ||
            !bus.registerObject(QStringLiteral("/org/freedesktop/Notifications"), &service,
                                QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals)) return 1;
        std::puts("ready");
        std::fflush(stdout);
        return app.exec();
    }
    if (argc == 2 && QByteArray(argv[1]) == "--service-switch") {
        NotificationService service(true);
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (!bus.registerService(QStringLiteral("org.freedesktop.Notifications")) ||
            !bus.registerObject(QStringLiteral("/org/freedesktop/Notifications"), &service,
                                QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals)) return 1;
        std::puts("ready");
        std::fflush(stdout);
        return app.exec();
    }
    if (argc == 2 && QByteArray(argv[1]) == "--service-wait") {
        NotificationService service;
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (!bus.registerObject(QStringLiteral("/org/freedesktop/Notifications"), &service,
                                QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals)) return 1;
        std::puts("waiting");
        std::fflush(stdout);
        QThread::msleep(250);
        while (!bus.registerService(QStringLiteral("org.freedesktop.Notifications")))
            QThread::msleep(5);
        return app.exec();
    }
    QProcess service;
    if (!require(startService(service), "service start")) return 1;
    NotificationHost host;
    const megler_notification_host callbacks = host.host();
    const auto send = [&](int op, const char *pkg, const char *tag, int id,
                          const char *title = nullptr, const char *body = nullptr) {
        return callbacks.send(callbacks.user, op, pkg, tag, id, title, body);
    };
    const char *pkg = "org.example.app";
    if (!require(send(MEGLER_NOTIFICATION_POST, pkg, nullptr, 7, "Title", "Body"), "first post")) return 1;
    QJsonObject first = record(service);
    if (!require(first.value("app").toString() == QLatin1String(pkg) &&
                 first.value("replaces").toInt(-1) == 0 &&
                 first.value("title").toString() == QLatin1String("Title") &&
                 first.value("body").toString() == QLatin1String("Body"), "first fields")) return 1;
    if (!require(send(MEGLER_NOTIFICATION_POST, pkg, nullptr, 7, "Updated", ""), "update")) return 1;
    if (!require(record(service).value("replaces").toInt() == 41, "replacement id")) return 1;
    const auto detailed = [&](int id, int flags, int max, int progress, bool indeterminate) {
        return callbacks.send_detailed(callbacks.user, pkg, nullptr, id, "Download", "Downloading",
                                       flags, max, progress, indeterminate,
                                       MEGLER_NOTIFICATION_IMPORTANCE_HIGH, false);
    };
    if (!require(detailed(10, 8, 100, 25, false), "progress post")) return 1;
    QJsonObject progress = record(service);
    if (!require(progress.value("replaces").toInt(-1) == 0 && progress.value("hasProgress").toBool() &&
                 progress.value("progress").toDouble() == 0.25 && progress.value("timeout").toInt() == 0 &&
                 !progress.value("previewSpecified").toBool(), "initial progress fields")) return 1;
    if (!require(detailed(10, 0, 100, 75, false), "progress update")) return 1;
    progress = record(service);
    if (!require(progress.value("replaces").toInt() == 42 && progress.value("progress").toDouble() == 0.75 &&
                 progress.value("previewSpecified").toBool() && progress.value("previewSummary").toString().isEmpty() &&
                 progress.value("previewBody").toString().isEmpty() && progress.value("suppressSound").toBool(),
                 "silent progress replacement")) return 1;
    if (!require(detailed(10, 8, 0, 0, true), "indeterminate progress")) return 1;
    progress = record(service);
    if (!require(progress.value("replaces").toInt() == 42 && progress.value("progress").toDouble() == -1,
                 "indeterminate progress fields")) return 1;
    if (!require(detailed(10, 0, 0, 0, false), "normal update")) return 1;
    progress = record(service);
    if (!require(progress.value("replaces").toInt() == 42 && !progress.value("hasProgress").toBool() &&
                 !progress.value("previewSpecified").toBool() && !progress.value("suppressSound").toBool(),
                 "normal updates remain alerting")) return 1;
    if (!require(detailed(10, 8, 0, 0, false), "progress completion")) return 1;
    progress = record(service);
    if (!require(progress.value("replaces").toInt() == 42 && !progress.value("hasProgress").toBool() &&
                 progress.value("timeout").toInt() == -1 && progress.value("suppressSound").toBool(),
                 "completion retains only-alert-once semantics")) return 1;
    if (!require(send(MEGLER_NOTIFICATION_CANCEL, pkg, nullptr, 10), "cancel progress")) return 1;
    if (!require(record(service).value("closed").toInt() == 42, "cancel progress id")) return 1;
    QDBusMessage dismiss = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("/org/freedesktop/Notifications"),
        QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("Dismiss"));
    dismiss << uint(41);
    if (!require(QDBusConnection::sessionBus().call(dismiss).type() == QDBusMessage::ReplyMessage,
                 "dismiss")) return 1;
    QEventLoop events;
    QTimer::singleShot(50, &events, &QEventLoop::quit);
    events.exec();
    if (!require(send(MEGLER_NOTIFICATION_POST, pkg, nullptr, 7, "Again", ""), "post after dismissal")) return 1;
    if (!require(record(service).value("replaces").toInt(-1) == 0, "dismissed id forgotten")) return 1;
    if (!require(send(MEGLER_NOTIFICATION_POST, pkg, "", 7, "Other", ""), "empty tag")) return 1;
    if (!require(record(service).value("replaces").toInt(-1) == 0, "null and empty tags differ")) return 1;
    if (!require(send(MEGLER_NOTIFICATION_CANCEL, pkg, nullptr, 7), "cancel")) return 1;
    if (!require(record(service).value("closed").toInt() == 43, "cancel id")) return 1;
    if (!require(send(MEGLER_NOTIFICATION_CANCEL_ALL, pkg, nullptr, 0), "cancel all")) return 1;
    if (!require(record(service).value("closed").toInt() == 44, "remaining id")) return 1;
    if (!require(!send(MEGLER_NOTIFICATION_POST, "org.example.other", nullptr, 7, "", ""),
                 "different package rejected")) return 1;
    const QByteArray oversized(1025, 'x');
    if (!require(!send(MEGLER_NOTIFICATION_POST, pkg, oversized.constData(), 7, "", ""),
                 "oversized tag rejected")) return 1;
    if (!require(send(MEGLER_NOTIFICATION_POST, pkg, nullptr, 7, "", ""), "post before restart")) return 1;
    record(service);
    service.terminate();
    if (!require(service.waitForFinished(5000), "service stop")) return 1;
    if (!require(!send(MEGLER_NOTIFICATION_POST, pkg, nullptr, 7, "", ""), "unavailable service")) return 1;
    if (!require(startService(service), "service restart")) return 1;
    if (!require(send(MEGLER_NOTIFICATION_POST, pkg, nullptr, 7, "", ""), "post after restart")) return 1;
    if (!require(record(service).value("replaces").toInt(-1) == 0, "stale id discarded")) return 1;
    const auto classified = [&](int id, int flags, int importance, bool silent, int max = 0, int value = 0) {
        return callbacks.send_detailed(callbacks.user, pkg, nullptr, id, "Download", "Downloading",
                                       flags, max, value, false, importance, silent);
    };
    if (!require(classified(20, 2, MEGLER_NOTIFICATION_IMPORTANCE_MIN, true),
                 "initial quiet ongoing notification")) return 1;
    QJsonObject policy = record(service);
    if (!require(policy.value("replaces").toInt(-1) == 0 &&
                 policy.value("previewSpecified").toBool() &&
                 policy.value("previewSummary").toString().isEmpty() &&
                 policy.value("previewBody").toString().isEmpty() &&
                 policy.value("suppressSound").toBool() &&
                 policy.value("originPackage").toString() == QLatin1String(pkg) &&
                 policy.value("hasUrgency").toBool() && policy.value("urgencyIsInt").toBool() &&
                 policy.value("urgency").toInt(-1) == 0 &&
                 policy.value("timeout").toInt(-1) == 0 && !policy.value("hasProgress").toBool(),
                 "initial ongoing minimum importance is quiet without progress")) return 1;
    const int quietId = 42;
    if (!require(classified(20, 2, MEGLER_NOTIFICATION_IMPORTANCE_LOW, false, 100, 25),
                 "quiet progress update")) return 1;
    policy = record(service);
    if (!require(policy.value("replaces").toInt() == quietId && policy.value("hasProgress").toBool() &&
                 policy.value("previewSpecified").toBool() && policy.value("suppressSound").toBool() &&
                 policy.value("urgency").toInt(-1) == 0,
                 "low importance remains quiet on progress")) return 1;
    if (!require(classified(21, 0, MEGLER_NOTIFICATION_IMPORTANCE_DEFAULT, false), "default post")) return 1;
    policy = record(service);
    if (!require(policy.value("previewSpecified").toBool() && !policy.value("suppressSound").toBool() &&
                 policy.value("urgency").toInt(-1) == 1 && policy.value("timeout").toInt() == -1,
                 "default importance retains sound without preview")) return 1;
    if (!require(classified(22, 0, MEGLER_NOTIFICATION_IMPORTANCE_HIGH, false), "high post")) return 1;
    policy = record(service);
    if (!require(!policy.value("previewSpecified").toBool() && !policy.value("suppressSound").toBool() &&
                 policy.value("urgency").toInt(-1) == 1,
                 "high importance permits preview")) return 1;
    if (!require(classified(23, 0, MEGLER_NOTIFICATION_IMPORTANCE_HIGH, true), "silent high post")) return 1;
    policy = record(service);
    if (!require(!policy.value("previewSpecified").toBool() && policy.value("suppressSound").toBool(),
                 "silent channel may still show preview")) return 1;
    if (!require(classified(24, 0, MEGLER_NOTIFICATION_IMPORTANCE_MAX, false), "max post")) return 1;
    policy = record(service);
    if (!require(!policy.value("previewSpecified").toBool() && !policy.value("suppressSound").toBool() &&
                 policy.value("urgency").toInt(-1) == 1, "max importance permits preview")) return 1;
    if (!require(classified(20, 0, MEGLER_NOTIFICATION_IMPORTANCE_HIGH, false),
                 "ongoing completion")) return 1;
    policy = record(service);
    if (!require(policy.value("replaces").toInt() == quietId && policy.value("timeout").toInt() == -1,
                 "completion clears ongoing timeout")) return 1;
    service.terminate();
    if (!require(service.waitForFinished(5000), "final service stop")) return 1;
    if (!require(startService(service, QStringLiteral("--service-switch")), "switch service start")) return 1;
    QProcess replacement;
    replacement.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--service-wait")});
    if (!require(replacement.waitForStarted(), "replacement service start")) return 1;
    if (!require(replacement.waitForReadyRead(5000) &&
                 replacement.readLine().trimmed() == "waiting", "replacement service wait")) return 1;
    if (!require(send(MEGLER_NOTIFICATION_POST, pkg, nullptr, 30, "Owner handover", "Recovered"),
                 "post retries after notification owner handover")) return 1;
    QJsonObject recovered = record(replacement);
    if (!require(recovered.value("title").toString() == QLatin1String("Owner handover") &&
                 recovered.value("body").toString() == QLatin1String("Recovered") &&
                 recovered.value("replaces").toInt(-1) == 0,
                 "notification retried with new owner")) return 1;
    replacement.terminate();
    if (!require(replacement.waitForFinished(5000), "replacement service stop")) return 1;
    service.terminate();
    if (!require(service.waitForFinished(5000), "switch service stop")) return 1;
    return 0;
}

#include "notification_host_test.moc"
