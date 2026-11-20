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
        QCoreApplication::translate("AndroidCompatibilityLayer", "Aurora WebView is unavailable.") !=
            QString::fromUtf8("Aurora WebView недоступен.") ||
        QCoreApplication::translate("YandexFormPage", "Yandex form") !=
            QString::fromUtf8("Яндекс Форма") ||
        QCoreApplication::translate("YandexFormPage", "Could not load the Yandex form.") !=
            QString::fromUtf8("Не удалось загрузить Яндекс Форму.") ||
        QCoreApplication::translate("YandexFormPage", "Select log file") !=
            QString::fromUtf8("Выберите файл журнала") ||
        QCoreApplication::translate("ExportThread", "The Downloads folder is unavailable.") !=
            QString::fromUtf8("Папка загрузок недоступна.") ||
        QCoreApplication::translate("RpmStandalone", "Cannot create the RPM output directory.") !=
            QString::fromUtf8("Не удалось создать каталог для сохранения RPM.") ||
        QCoreApplication::translate("StandaloneRuntimeError",
            "The shared Android runtime is incompatible. Recreate the RPM in the manager and reinstall it.") !=
            QString::fromUtf8("Общая среда Android несовместима с приложением. Создайте RPM заново в менеджере и переустановите его.") ||
        QCoreApplication::translate("StandaloneRuntimeError",
            "The shared Android runtime is unavailable. This app requires the manager and access to its runtime. The manager and this app must be signed with the same key.") !=
            QString::fromUtf8("Общая среда Android недоступна. Для работы приложения нужны менеджер и доступ к его среде. Менеджер и приложение должны быть подписаны одним ключом.") ||
        QCoreApplication::translate("Probe",
            "The export is partial: some standalone snapshots were unavailable and remain in the organization folder.") !=
            QString::fromUtf8("Экспорт неполный: некоторые снимки журналов standalone-приложений недоступны и остаются в папке организации.") ||
        QCoreApplication::translate("ErrorLog",
            "Export succeeded, but some organization snapshots could not be removed.") !=
            QString::fromUtf8("Экспорт выполнен, но некоторые снимки журналов в папке организации не удалось удалить.") ||
        !QCoreApplication::translate("Probe", "Could not start %1: %2").arg("a", "b")
            .contains(QStringLiteral("a: b"))) return 1;
    app.removeTranslator(&translator);
    if (QCoreApplication::translate("AndroidCompatibilityLayer", "Add APK or bundle") !=
        QStringLiteral("Add APK or bundle")) return 1;
    std::puts("Translations OK");
    return 0;
}
