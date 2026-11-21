#ifndef ACL_STANDALONE_MICROG_DIAGNOSTIC_H
#define ACL_STANDALONE_MICROG_DIAGNOSTIC_H

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusReply>

namespace StandaloneMicrogDiagnostic {
static const char page[] = R"QML(
import QtQuick 2.0
Rectangle {
    color: "#20242a"
    signal continueRequested()
    Flickable {
        anchors.fill: parent
        objectName: "scroll"
        contentHeight: content.height + 40
        clip: true
        Column {
            id: content
            objectName: "content"
            x: 20; y: 20
            width: parent.width - 40
            spacing: 16
            Text { width: parent.width; color: "white"; wrapMode: Text.Wrap
                font.pixelSize: 24; text: "Standalone synthetic IPC test" }
            Text { width: parent.width; color: "white"; wrapMode: Text.Wrap
                font.pixelSize: 20; text: "Owner before request: " + ownerStatus }
            Text { width: parent.width; color: "white"; wrapMode: Text.Wrap
                font.pixelSize: 20; text: "DiagnosticPing: " + replyStatus }
            Text { width: parent.width; color: "white"; wrapMode: Text.Wrap
                font.pixelSize: 18; text: "This does not test a Google service." }
            Rectangle {
                objectName: "continueButton"
                width: Math.min(180, parent.width); height: 56; color: "#4678a8"
                Text { anchors.centerIn: parent; color: "white"; font.pixelSize: 20; text: "Continue" }
                MouseArea { anchors.fill: parent; onClicked: continueRequested() }
            }
        }
    }
}
)QML";

struct Result {
    QString owner;
    QString response;
};

inline Result run(const QDBusConnection &bus)
{
    const QString service = QStringLiteral("ru.androidcompatibilitylayer.appmanager");
    Result result;
    if (!bus.isConnected() || !bus.interface()) {
        result.owner = QStringLiteral("error: session bus unavailable");
        result.response = QStringLiteral("not sent");
        return result;
    }
    const QDBusReply<bool> owner = bus.interface()->isServiceRegistered(service);
    if (!owner.isValid()) {
        result.owner = QStringLiteral("error: ") + owner.error().name();
        result.response = QStringLiteral("not sent");
        return result;
    }
    result.owner = owner.value() ? QStringLiteral("present") : QStringLiteral("absent");
    QDBusMessage message = QDBusMessage::createMethodCall(service,
        QStringLiteral("/ru/androidcompatibilitylayer/appmanager"), service,
        QStringLiteral("DiagnosticPing"));
    const QDBusMessage reply = bus.call(message, QDBus::Block, 15000);
    if (reply.type() == QDBusMessage::ErrorMessage)
        result.response = QStringLiteral("error: ") + reply.errorName();
    else if (reply.type() == QDBusMessage::ReplyMessage && reply.arguments().size() == 1 &&
             reply.arguments().first().type() == QVariant::String &&
             reply.arguments().first().toString() == QLatin1String("synthetic-ok"))
        result.response = QStringLiteral("synthetic-ok");
    else
        result.response = QStringLiteral("error: invalid response");
    return result;
}
}
#endif
