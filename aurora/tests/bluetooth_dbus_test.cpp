#include "../bluetooth_dbus.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>

#include <dbus/dbus.h>

#include <atomic>
#include <cstdio>
#include <thread>

namespace {

std::atomic<int> readOnlyWriteWarnings(0);

void captureQtMessage(QtMsgType, const QMessageLogContext &, const QString &message)
{
    if (message.contains(QLatin1String("QDBusArgument: write from a read-only object")))
        ++readOnlyWriteWarnings;
}

bool appendStringProperty(DBusMessageIter *properties, const char *name, const char *value)
{
    DBusMessageIter entry, variant;
    const char *key = name;
    if (!dbus_message_iter_open_container(properties, DBUS_TYPE_DICT_ENTRY, nullptr, &entry) ||
        !dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key) ||
        !dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant) ||
        !dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &value) ||
        !dbus_message_iter_close_container(&entry, &variant))
        return false;
    return dbus_message_iter_close_container(properties, &entry);
}

bool appendBooleanProperty(DBusMessageIter *properties, const char *name, bool value)
{
    DBusMessageIter entry, variant;
    const char *key = name;
    const dbus_bool_t boolean = value ? TRUE : FALSE;
    if (!dbus_message_iter_open_container(properties, DBUS_TYPE_DICT_ENTRY, nullptr, &entry) ||
        !dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key) ||
        !dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &variant) ||
        !dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &boolean) ||
        !dbus_message_iter_close_container(&entry, &variant))
        return false;
    return dbus_message_iter_close_container(properties, &entry);
}

bool appendUuidProperty(DBusMessageIter *properties)
{
    DBusMessageIter entry, variant, uuids;
    const char *key = "UUIDs";
    const char *uuid = "0000180f-0000-1000-8000-00805f9b34fb";
    if (!dbus_message_iter_open_container(properties, DBUS_TYPE_DICT_ENTRY, nullptr, &entry) ||
        !dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key) ||
        !dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "as", &variant) ||
        !dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "s", &uuids) ||
        !dbus_message_iter_append_basic(&uuids, DBUS_TYPE_STRING, &uuid) ||
        !dbus_message_iter_close_container(&variant, &uuids) ||
        !dbus_message_iter_close_container(&entry, &variant))
        return false;
    return dbus_message_iter_close_container(properties, &entry);
}

DBusMessage *managedObjectsReply(DBusMessage *call)
{
    DBusMessage *reply = dbus_message_new_method_return(call);
    if (!reply) return nullptr;

    DBusMessageIter root, objects, objectEntry, interfaces, interfaceEntry, properties;
    const char *path = "/org/bluez/hci0";
    const char *interfaceName = "org.bluez.Adapter1";
    dbus_message_iter_init_append(reply, &root);
    const bool success =
        dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "{oa{sa{sv}}}", &objects) &&
        dbus_message_iter_open_container(&objects, DBUS_TYPE_DICT_ENTRY, nullptr, &objectEntry) &&
        dbus_message_iter_append_basic(&objectEntry, DBUS_TYPE_OBJECT_PATH, &path) &&
        dbus_message_iter_open_container(&objectEntry, DBUS_TYPE_ARRAY, "{sa{sv}}", &interfaces) &&
        dbus_message_iter_open_container(&interfaces, DBUS_TYPE_DICT_ENTRY, nullptr, &interfaceEntry) &&
        dbus_message_iter_append_basic(&interfaceEntry, DBUS_TYPE_STRING, &interfaceName) &&
        dbus_message_iter_open_container(&interfaceEntry, DBUS_TYPE_ARRAY, "{sv}", &properties) &&
        appendBooleanProperty(&properties, "Powered", true) &&
        appendStringProperty(&properties, "Name", "test-adapter") &&
        appendUuidProperty(&properties) &&
        dbus_message_iter_close_container(&interfaceEntry, &properties) &&
        dbus_message_iter_close_container(&interfaces, &interfaceEntry) &&
        dbus_message_iter_close_container(&objectEntry, &interfaces) &&
        dbus_message_iter_close_container(&objects, &objectEntry) &&
        dbus_message_iter_close_container(&root, &objects);
    if (!success) {
        dbus_message_unref(reply);
        return nullptr;
    }
    return reply;
}

DBusHandlerResult handleMessage(DBusConnection *connection, DBusMessage *message, void *)
{
    if (!dbus_message_is_method_call(message, "org.freedesktop.DBus.ObjectManager", "GetManagedObjects"))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    DBusMessage *reply = managedObjectsReply(message);
    if (!reply) return DBUS_HANDLER_RESULT_NEED_MEMORY;
    dbus_connection_send(connection, reply, nullptr);
    dbus_connection_flush(connection);
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_HANDLED;
}

}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (!dbus_threads_init_default()) {
        std::fprintf(stderr, "bluetooth_dbus_test: libdbus thread initialization failed\n");
        return 1;
    }

    DBusError error;
    dbus_error_init(&error);
    DBusConnection *service = dbus_bus_get(DBUS_BUS_SESSION, &error);
    if (!service) {
        std::fprintf(stderr, "bluetooth_dbus_test: cannot connect mock service: %s\n", error.message);
        dbus_error_free(&error);
        return 1;
    }
    dbus_connection_set_exit_on_disconnect(service, FALSE);
    const int nameResult = dbus_bus_request_name(service, "org.bluez", DBUS_NAME_FLAG_DO_NOT_QUEUE, &error);
    if (nameResult != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        std::fprintf(stderr, "bluetooth_dbus_test: cannot own org.bluez: %s\n",
                     dbus_error_is_set(&error) ? error.message : "name is already owned");
        dbus_error_free(&error);
        dbus_connection_unref(service);
        return 1;
    }

    DBusObjectPathVTable vtable = {};
    vtable.message_function = handleMessage;
    if (!dbus_connection_register_object_path(service, "/", &vtable, nullptr)) {
        std::fprintf(stderr, "bluetooth_dbus_test: cannot register mock ObjectManager\n");
        dbus_connection_unref(service);
        return 1;
    }

    std::atomic<bool> dispatching(true);
    std::thread serviceThread([&] {
        while (dispatching.load())
            if (!dbus_connection_read_write_dispatch(service, 50)) break;
    });

    const QDBusConnection bus = QDBusConnection::sessionBus();
    const QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.bluez"), QStringLiteral("/"),
        QStringLiteral("org.freedesktop.DBus.ObjectManager"), QStringLiteral("GetManagedObjects"));
    const QDBusMessage reply = bus.call(call, QDBus::Block, 3000);
    dispatching.store(false);
    serviceThread.join();
    dbus_connection_unregister_object_path(service, "/");
    dbus_bus_release_name(service, "org.bluez", nullptr);
    dbus_connection_unref(service);

    if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty()) {
        std::fprintf(stderr, "bluetooth_dbus_test: mock GetManagedObjects call failed: %s\n",
                     reply.errorMessage().toLocal8Bit().constData());
        return 1;
    }

    qInstallMessageHandler(captureQtMessage);
    AuroraBluetooth::ManagedObjects objects;
    if (!AuroraBluetooth::readManagedObjects(reply.arguments().first(), &objects)) {
        qInstallMessageHandler(nullptr);
        std::fprintf(stderr, "bluetooth_dbus_test: could not parse GetManagedObjects reply\n");
        return 1;
    }
    qInstallMessageHandler(nullptr);
    const auto adapter = objects.value(QStringLiteral("/org/bluez/hci0"));
    const auto properties = adapter.value(QStringLiteral("org.bluez.Adapter1"));
    const QVariant powered = properties.value(QStringLiteral("Powered")).value<QDBusVariant>().variant();
    const QVariant name = properties.value(QStringLiteral("Name")).value<QDBusVariant>().variant();
    const QVariant uuids = properties.value(QStringLiteral("UUIDs")).value<QDBusVariant>().variant();
    if (readOnlyWriteWarnings.load() != 0 || objects.size() != 1 || adapter.isEmpty() || !powered.toBool() ||
        name.toString() != QStringLiteral("test-adapter") || uuids.toStringList().size() != 1) {
        std::fprintf(stderr, "bluetooth_dbus_test: parsed properties mismatch (warnings=%d objects=%d keys=%s interfaces=%d properties=%d powered=%s name=%s uuids=%s)\n",
                     readOnlyWriteWarnings.load(),
                     objects.size(), objects.keys().join(QLatin1Char(',')).toLocal8Bit().constData(),
                     adapter.size(), properties.size(), powered.toString().toLocal8Bit().constData(),
                     name.toString().toLocal8Bit().constData(),
                     uuids.toStringList().join(QLatin1Char(',')).toLocal8Bit().constData());
        return 1;
    }

    std::puts("bluetooth_dbus_test: passed");
    return 0;
}
