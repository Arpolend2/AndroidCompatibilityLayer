#include "../external_apk_receiver.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QEventLoop>
#include <QTimer>

#include <cstdio>

int main(int argc, char **argv)
{
    qputenv("AURORA_APP_ID", "ru.megler.MeglerProbe");
    qputenv("AURORA_APP_INSTANCE_ID", "ru.megler.MeglerProbe");
    QCoreApplication app(argc, argv);
    ExternalApkReceiver receiver;
    QString path, rejection, error;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&receiver, &ExternalApkReceiver::requested, &loop, [&](const QString &value) {
        path = value;
        loop.quit();
    });
    QObject::connect(&receiver, &ExternalApkReceiver::rejected, &loop, [&](const QString &value) {
        rejection = value;
        loop.quit();
    });
    if (!receiver.start(&error)) {
        std::fprintf(stderr, "Cannot start receiver: %s\n", qPrintable(error));
        return 1;
    }

    QDBusConnection bus = QDBusConnection::sessionBus();
    QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("ru.megler.MeglerProbe"),
        QStringLiteral("/ru/megler/MeglerProbe"), QStringLiteral("ru.megler.MeglerProbe"),
        QStringLiteral("openFile"));
    call << (QStringList() << QStringLiteral("file:///home/user/My%20app.apk"));
    if (!bus.send(call)) return 1;
    timeout.start(5000);
    if (path.isEmpty() && rejection.isEmpty()) loop.exec();
    timeout.stop();
    if (path != QLatin1String("/home/user/My app.apk") || !rejection.isEmpty()) return 1;

    QDBusMessage introspect = QDBusMessage::createMethodCall(QStringLiteral("ru.megler.MeglerProbe"),
        QStringLiteral("/ru/omp/RuntimeManager/IntentHandler1"), QStringLiteral("org.freedesktop.DBus.Introspectable"),
        QStringLiteral("Introspect"));
    QDBusPendingCallWatcher watcher(bus.asyncCall(introspect));
    QObject::connect(&watcher, &QDBusPendingCallWatcher::finished, &loop, &QEventLoop::quit);
    timeout.start(5000);
    if (!watcher.isFinished()) loop.exec();
    timeout.stop();
    if (!watcher.isFinished()) {
        std::fprintf(stderr, "RuntimeManager introspection timed out\n");
        return 1;
    }
    QDBusPendingReply<QString> reply = watcher.reply();
    if (reply.isError() || !reply.value().contains(QLatin1String("HandleIntent"))) {
        std::fprintf(stderr, "RuntimeManager handler is not available\n");
        return 1;
    }
    path.clear();
    QDBusMessage intent = QDBusMessage::createMethodCall(QStringLiteral("ru.megler.MeglerProbe"),
        QStringLiteral("/ru/omp/RuntimeManager/IntentHandler1"),
        QStringLiteral("ru.omp.RuntimeManager.IntentHandler1"), QStringLiteral("HandleIntent"));
    QVariantMap params;
    params.insert(QStringLiteral("uri"), QStringLiteral("file:///home/user/from%20intent.apk"));
    intent << QStringLiteral("OpenURI") << params;
    QDBusPendingCallWatcher intentWatcher(bus.asyncCall(intent));
    QObject::connect(&intentWatcher, &QDBusPendingCallWatcher::finished, &loop, &QEventLoop::quit);
    timeout.start(5000);
    if (!intentWatcher.isFinished()) loop.exec();
    while (!intentWatcher.isFinished() && timeout.isActive()) loop.exec();
    timeout.stop();
    if (!intentWatcher.isFinished()) {
        std::fprintf(stderr, "RuntimeManager OpenURI reply timed out\n");
        return 1;
    }
    const QDBusMessage intentReply = intentWatcher.reply();
    if (intentReply.type() == QDBusMessage::ErrorMessage) {
        std::fprintf(stderr, "RuntimeManager OpenURI D-Bus error: %s: %s\n",
                     qPrintable(intentReply.errorName()), qPrintable(intentReply.errorMessage()));
        return 1;
    }
    if (path != QLatin1String("/home/user/from intent.apk") || !rejection.isEmpty()) {
        std::fprintf(stderr, "RuntimeManager OpenURI request was not delivered (path=%s, rejection=%s)\n",
                     qPrintable(path), qPrintable(rejection));
        return 1;
    }

    path.clear();
    receiver.openFile(QStringList() << QStringLiteral("file:///home/user/a.XAPK"));
    if (path != QLatin1String("/home/user/a.XAPK") || !rejection.isEmpty()) return 1;
    path.clear();
    receiver.submit(QStringList() << QStringLiteral("file:///home/user/b.apks"));
    if (path != QLatin1String("/home/user/b.apks") || !rejection.isEmpty()) return 1;
    path.clear();
    receiver.openFile(QStringList() << QStringLiteral("file:///home/user/a.apk")
                                    << QStringLiteral("file:///home/user/b.apk"));
    if (!path.isEmpty() || rejection.isEmpty()) return 1;
    std::puts("External APK D-Bus receiver OK");
    return 0;
}
