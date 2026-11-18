#include "../runtime_settings.h"

#include <QCoreApplication>
#include <QTemporaryDir>

#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("AndroidCompatibilityLayerTest"));
    QCoreApplication::setApplicationName(QStringLiteral("RuntimeSettings"));
    QTemporaryDir directory;
    if (!directory.isValid()) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    if (RuntimeSettings::renderCap() != 0 ||
        !RuntimeSettings::selectedLocale().isEmpty() ||
        RuntimeSettings::locale() != RuntimeSettings::systemLocaleTag(QLocale::system()) ||
        RuntimeSettings::systemLocaleTag(QLocale(QStringLiteral("ru_RU"))) != QStringLiteral("ru-RU") ||
        RuntimeSettings::systemLocaleTag(QLocale(QStringLiteral("sr_Latn_RS"))) != QStringLiteral("sr-Latn-RS") ||
        RuntimeSettings::systemLocaleTag(QLocale::c()) != QStringLiteral("en-US") ||
        !RuntimeSettings::theme().isEmpty()) return 1;
    if (!RuntimeSettings::validLocale(QStringLiteral("zh-Hant-TW")) ||
        RuntimeSettings::validLocale(QStringLiteral("en--US")) ||
        RuntimeSettings::validLocale(QStringLiteral("1x-US")) ||
        RuntimeSettings::validLocale(QString(36, QLatin1Char('a')))) return 1;
    if (!RuntimeSettings::save(QStringLiteral("runtime/renderCap"), 960) ||
        !RuntimeSettings::save(QStringLiteral("runtime/locale"), QStringLiteral("ru-RU")) ||
        RuntimeSettings::renderCap() != 960 || RuntimeSettings::locale() != QStringLiteral("ru-RU") ||
        RuntimeSettings::selectedLocale() != QStringLiteral("ru-RU")) return 1;
    if (!RuntimeSettings::save(QStringLiteral("runtime/locale"), QString()) ||
        !RuntimeSettings::selectedLocale().isEmpty() ||
        RuntimeSettings::locale() != RuntimeSettings::systemLocaleTag(QLocale::system())) return 1;
    if (!RuntimeSettings::save(QStringLiteral("runtime/theme"), QStringLiteral("dark")) ||
        RuntimeSettings::theme() != QStringLiteral("dark") ||
        RuntimeSettings::validTheme(QStringLiteral("invalid"))) return 1;
    double latitude = 0, longitude = 0;
    if (!RuntimeSettings::parseLocation(QStringLiteral("55.751244, 37.618423"), &latitude, &longitude) ||
        latitude != 55.751244 || longitude != 37.618423 ||
        RuntimeSettings::parseLocation(QStringLiteral("91,0"), nullptr, nullptr) ||
        RuntimeSettings::parseLocation(QStringLiteral("0,nan"), nullptr, nullptr) ||
        RuntimeSettings::parseLocation(QStringLiteral("0,0,1"), nullptr, nullptr) ||
        !RuntimeSettings::location().isEmpty()) return 1;
    if (!RuntimeSettings::save(QStringLiteral("runtime/location"), QStringLiteral("55.5, 37.5")) ||
        !RuntimeSettings::save(QStringLiteral("runtime/theme"), QStringLiteral("light")) ||
        RuntimeSettings::location() != QStringLiteral("55.5, 37.5")) return 1;
    std::puts("Runtime settings OK");
    return 0;
}
