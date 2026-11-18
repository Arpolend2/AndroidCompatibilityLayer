#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_RUNTIME_SETTINGS_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_RUNTIME_SETTINGS_H

#include <QDebug>
#include <QLocale>
#include <QRegExp>
#include <QSettings>
#include <QString>

#include <cmath>

namespace RuntimeSettings {

inline bool validRenderCap(int value)
{
    return value == 0 || value == 960 || value == 1280 || value == 1600;
}

inline bool validLocale(const QString &value)
{
    return value.size() < 36 &&
           QRegExp(QStringLiteral("[A-Za-z]{2,3}(-[A-Za-z0-9]{1,8})*")).exactMatch(value);
}

inline int renderCap()
{
    QSettings settings;
    bool ok = false;
    const int value = settings.value(QStringLiteral("runtime/renderCap"), 0).toInt(&ok);
    if (!ok || !validRenderCap(value)) {
        qWarning() << "Invalid render size setting; using native size";
        return 0;
    }
    return value;
}

inline QString selectedLocale()
{
    QSettings settings;
    const QString value = settings.value(QStringLiteral("runtime/locale")).toString();
    if (!value.isEmpty() && !validLocale(value)) {
        qWarning() << "Invalid language setting; using system language";
        return QString();
    }
    return value;
}

inline QString systemLocaleTag(const QLocale &systemLocale)
{
    QString value = systemLocale.name();
    value.replace(QLatin1Char('_'), QLatin1Char('-'));
    if (!validLocale(value)) {
        qWarning() << "System language is unavailable; using en-US";
        return QStringLiteral("en-US");
    }
    const QStringList parts = systemLocale.bcp47Name().split(QLatin1Char('-'));
    for (int i = 1; i < parts.size(); ++i) {
        if (parts.at(i).size() == 4) {
            const int region = value.indexOf(QLatin1Char('-'));
            if (region >= 0) value.insert(region, QLatin1Char('-') + parts.at(i));
            else value += QLatin1Char('-') + parts.at(i);
            break;
        }
    }
    return value;
}

inline QString localeForSelection(const QString &selected)
{
    return selected.isEmpty() ? systemLocaleTag(QLocale::system()) : selected;
}

inline QString locale()
{
    return localeForSelection(selectedLocale());
}

inline bool validTheme(const QString &value)
{
    return value.isEmpty() || value == QLatin1String("light") || value == QLatin1String("dark");
}

inline QString theme()
{
    const QString value = QSettings().value(QStringLiteral("runtime/theme")).toString();
    if (!validTheme(value)) {
        qWarning() << "Invalid theme setting; using system theme";
        return QString();
    }
    return value;
}

inline bool parseLocation(const QString &value, double *latitude, double *longitude)
{
    const QStringList parts = value.split(QLatin1Char(','));
    if (parts.size() != 2) return false;
    bool latOk = false, lonOk = false;
    const double lat = parts.at(0).trimmed().toDouble(&latOk);
    const double lon = parts.at(1).trimmed().toDouble(&lonOk);
    if (!latOk || !lonOk || !std::isfinite(lat) || !std::isfinite(lon) ||
        lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) return false;
    if (latitude) *latitude = lat;
    if (longitude) *longitude = lon;
    return true;
}

inline QString location()
{
    const QString value = QSettings().value(QStringLiteral("runtime/location")).toString();
    if (!value.isEmpty() && !parseLocation(value, nullptr, nullptr)) {
        qWarning() << "Invalid location setting; using device location";
        return QString();
    }
    return value;
}

inline bool save(const QString &key, const QVariant &value)
{
    QSettings settings;
    const bool existed = settings.contains(key);
    const QVariant previous = settings.value(key);
    settings.setValue(key, value);
    settings.sync();
    if (settings.status() == QSettings::NoError) return true;
    if (existed) settings.setValue(key, previous);
    else settings.remove(key);
    settings.sync();
    qWarning() << "Could not save runtime setting" << key;
    return false;
}

}

#endif
