#include <QCoreApplication>
#include <QString>
#include <QTranslator>

#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 2) return 2;
    QTranslator translator;
    if (!translator.load(QString::fromLocal8Bit(argv[1])) || !app.installTranslator(&translator)) return 1;
    if (QCoreApplication::translate("AndroidCompatibilityLayer", "Add APK or bundle") !=
            QString::fromUtf8("Добавить установочный пакет") ||
        QCoreApplication::translate("Preview", "Close") != QString::fromUtf8("Закрыть") ||
        QCoreApplication::translate("AndroidCompatibilityLayer", "About") != QString::fromUtf8("О приложении") ||
        QCoreApplication::translate("AndroidCompatibilityLayer", "Resolution") != QString::fromUtf8("Разрешение") ||
        QCoreApplication::translate("AndroidCompatibilityLayer", "Native") != QString::fromUtf8("Родное") ||
        QCoreApplication::translate("ExportThread", "The Downloads folder is unavailable.") !=
            QString::fromUtf8("Папка загрузок недоступна.") ||
        QCoreApplication::translate("RpmStandalone", "Cannot create the RPM output directory.") !=
            QString::fromUtf8("Не удалось создать каталог для сохранения RPM.") ||
        !QCoreApplication::translate("Probe", "Could not start %1: %2").arg("a", "b")
            .contains(QStringLiteral("a: b"))) return 1;
    app.removeTranslator(&translator);
    if (QCoreApplication::translate("AndroidCompatibilityLayer", "Add APK or bundle") !=
        QStringLiteral("Add APK or bundle")) return 1;
    std::puts("Translations OK");
    return 0;
}
