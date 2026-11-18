#ifndef MEGLER_AURORA_PERMISSION_SETTINGS_H
#define MEGLER_AURORA_PERMISSION_SETTINGS_H

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QSaveFile>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

namespace PermissionSettings {

struct Group {
    const char *name;
    const char *title;
    const char *permissions;
};

inline const QList<Group> &groups()
{
    static const QList<Group> value = {
        {"calendar", QT_TRANSLATE_NOOP("PermissionSettings", "Calendar"), "READ_CALENDAR WRITE_CALENDAR"},
        {"call_log", QT_TRANSLATE_NOOP("PermissionSettings", "Call logs"), "READ_CALL_LOG WRITE_CALL_LOG PROCESS_OUTGOING_CALLS"},
        {"camera", QT_TRANSLATE_NOOP("PermissionSettings", "Camera"), "CAMERA"},
        {"contacts", QT_TRANSLATE_NOOP("PermissionSettings", "Contacts"), "READ_CONTACTS WRITE_CONTACTS GET_ACCOUNTS"},
        {"location", QT_TRANSLATE_NOOP("PermissionSettings", "Location"), "ACCESS_FINE_LOCATION ACCESS_COARSE_LOCATION ACCESS_BACKGROUND_LOCATION"},
        {"microphone", QT_TRANSLATE_NOOP("PermissionSettings", "Microphone"), "RECORD_AUDIO"},
        {"phone", QT_TRANSLATE_NOOP("PermissionSettings", "Phone"), "READ_PHONE_STATE READ_PHONE_NUMBERS CALL_PHONE ANSWER_PHONE_CALLS ADD_VOICEMAIL USE_SIP ACCEPT_HANDOVER"},
        {"sensors", QT_TRANSLATE_NOOP("PermissionSettings", "Body sensors"), "BODY_SENSORS BODY_SENSORS_BACKGROUND"},
        {"sms", QT_TRANSLATE_NOOP("PermissionSettings", "SMS"), "SEND_SMS RECEIVE_SMS READ_SMS RECEIVE_WAP_PUSH RECEIVE_MMS READ_CELL_BROADCASTS"},
        {"storage", QT_TRANSLATE_NOOP("PermissionSettings", "Files and media"), "READ_EXTERNAL_STORAGE WRITE_EXTERNAL_STORAGE MANAGE_EXTERNAL_STORAGE"},
        {"read_media_visual", QT_TRANSLATE_NOOP("PermissionSettings", "Photos and videos"), "READ_MEDIA_IMAGES READ_MEDIA_VIDEO READ_MEDIA_VISUAL_USER_SELECTED ACCESS_MEDIA_LOCATION"},
        {"read_media_aural", QT_TRANSLATE_NOOP("PermissionSettings", "Music and audio"), "READ_MEDIA_AUDIO"},
        {"nearby_devices", QT_TRANSLATE_NOOP("PermissionSettings", "Nearby devices"), "BLUETOOTH BLUETOOTH_ADMIN BLUETOOTH_SCAN BLUETOOTH_CONNECT BLUETOOTH_ADVERTISE NEARBY_WIFI_DEVICES UWB_RANGING"},
        {"notifications", QT_TRANSLATE_NOOP("PermissionSettings", "Notifications"), "POST_NOTIFICATIONS"},
        {"activity_recognition", QT_TRANSLATE_NOOP("PermissionSettings", "Physical activity"), "ACTIVITY_RECOGNITION"}
    };
    return value;
}

inline bool known(const QString &name)
{
    for (const Group &group : groups())
        if (name == QLatin1String(group.name)) return true;
    return false;
}

inline bool parse(const QJsonValue &value, QStringList *denied)
{
    if (!denied || !value.isArray()) return false;
    QStringList result;
    for (const QJsonValue &item : value.toArray()) {
        if (!item.isString() || !known(item.toString()) || result.contains(item.toString())) return false;
        result.append(item.toString());
    }
    *denied = result;
    return true;
}

inline bool read(const QString &directory, QStringList *denied, QString *error)
{
    if (error) error->clear();
    if (denied) denied->clear();
    if (directory.isEmpty() || !denied || QFileInfo(directory).isSymLink() || !QFileInfo(directory).isDir()) {
        if (error) *error = QObject::tr("App permission settings directory is unavailable.");
        return false;
    }
    const QString path = QDir(directory).filePath(QStringLiteral("permissions.json"));
    if (QFileInfo(path).isSymLink()) {
        if (error) *error = QObject::tr("Permission settings are a symbolic link.");
        return false;
    }
    QFile file(path);
    if (!file.exists()) return true;
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4096) {
        if (error) *error = QObject::tr("Cannot read permission settings.");
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (file.error() != QFile::NoError || parseError.error != QJsonParseError::NoError ||
        !document.isObject() || !parse(document.object().value(QStringLiteral("denied")), denied)) {
        if (error) *error = QObject::tr("Invalid permission settings.");
        return false;
    }
    return true;
}

inline bool save(const QString &directory, const QStringList &denied, QString *error)
{
    QJsonArray array;
    for (const QString &group : denied) array.append(group);
    QStringList validated;
    if (!parse(array, &validated) || QFileInfo(directory).isSymLink() || !QFileInfo(directory).isDir() ||
        QFileInfo(QDir(directory).filePath(QStringLiteral("permissions.json"))).isSymLink()) {
        if (error) *error = QObject::tr("Invalid permission settings or app directory.");
        return false;
    }
    QJsonObject object;
    object.insert(QStringLiteral("denied"), array);
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    QSaveFile file(QDir(directory).filePath(QStringLiteral("permissions.json")));
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        if (error) *error = QObject::tr("Cannot save permission settings: %1").arg(file.errorString());
        return false;
    }
    return true;
}

inline QVariantList requested(const QVariantList &permissions, const QVariantList &maxSdk,
                              const QStringList &denied)
{
    QStringList names;
    for (int i = 0; i < permissions.size(); ++i) {
        const int max = i < maxSdk.size() ? maxSdk.at(i).toInt() : 0;
        const QString name = permissions.at(i).toString();
        if ((max == 0 || max >= 34) && name.startsWith(QLatin1String("android.permission.")))
            names.append(name.mid(19));
    }
    QVariantList result;
    for (const Group &group : groups()) {
        const QStringList members = QString::fromLatin1(group.permissions).split(QLatin1Char(' '));
        bool found = false;
        for (const QString &member : members) {
            if (member == QLatin1String("ACCESS_MEDIA_LOCATION") && names.contains(QStringLiteral("READ_EXTERNAL_STORAGE")))
                continue;
            if (names.contains(member)) found = true;
        }
        if (QLatin1String(group.name) == QLatin1String("storage") && names.contains(QStringLiteral("READ_EXTERNAL_STORAGE")) &&
            names.contains(QStringLiteral("ACCESS_MEDIA_LOCATION"))) found = true;
        if (!found) continue;
        QVariantMap item;
        item.insert(QStringLiteral("group"), QString::fromLatin1(group.name));
        item.insert(QStringLiteral("title"), QCoreApplication::translate("PermissionSettings", group.title));
        item.insert(QStringLiteral("allowed"), !denied.contains(QString::fromLatin1(group.name)));
        result.append(item);
    }
    return result;
}

}
#endif
