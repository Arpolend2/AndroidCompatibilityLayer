# AndroidCompatibilityLayer

**Порт проекта [Megler](https://github.com/Computershik73/megler-w10m) под ОС Аврора. Автор оригинального проекта — [Илья Высоцкий (Computershik73)](https://github.com/Computershik73).**

Добавляйте Android-приложения в библиотеку, настраивайте разрешения и создавайте самостоятельные RPM.

Проект экспериментальный: совместимость приложений не гарантируется. Подписи APK не проверяются, а полной изоляции Android-приложений нет — устанавливайте только доверенные файлы.

Лицензия: [GPL-3.0](LICENSE).

## Возможности

- Импорт APK, APKS и XAPK в библиотеку, в том числе через системное открытие файлов.
- Создание самостоятельных RPM для Android-приложений с выбором разрешений.
- [Aurora Store](https://gitlab.com/AuroraOSS/AuroraStore) внутри менеджера: установка приложений и открытие установленных самостоятельных приложений. Поддерживается перезапуск магазина.
- Опциональные настройки [microG](https://github.com/microg/GmsCore) внутри менеджера.
- Экспериментальная графика на Qt и GLES, мосты камеры, датчиков, Bluetooth LE, сети и звука.
- Обложка с приложениями и быстрыми действиями; журнал диагностики с опцией расширенного логирования.

Aurora Store, настройки microG и самостоятельный RuStore запускались на устройстве; поддержка других приложений зависит от используемых ими Android API.

## Ограничения

- Android System WebView и воспроизведение видео пока недоступны. В текущей версии войти в аккаунт Google через microG нельзя.
- microG остаётся экспериментальным: настройки в менеджере не означают общего входа для самостоятельных приложений.
- GPU-ускорение экспериментальное. В RPM-спецификации оно включено, а при отдельной сборке CMake выключено по умолчанию. На проверенном устройстве RuStore выводил кадры через общую GLES-текстуру. Если DMA-BUF недоступен, менеджер пробует Hybris gralloc/native handle; при его ошибке остаётся резервный путь через CPU.
- Мосты устройства поддерживают не все функции Android, а работа зависит от драйверов и разрешений Авроры.
- Расширенное логирование можно включить в настройках журнала. Используйте его осторожно: записи могут содержать личные данные, пути и сетевые адреса. Проверяйте журнал перед отправкой.

## Сборка

Подготовьте SQLite и Android runtime по [инструкции](aurora/README.md), затем откройте корневой `CMakeLists.txt` в Aurora IDE, выберите комплект Aurora SDK для архитектуры и версии ОС устройства, соберите и установите `ru.androidcompatibilitylayer.appmanager`. Самостоятельные RPM содержат собственный runtime; для получения обновлений его нужно пересоздать из новой сборки менеджера.

Для воспроизводимой кросс-сборки `armv7hl` и `aarch64` без `sb2`, chroot и `sudo` используйте [переносимые скрипты `tools/aurora`](tools/aurora/README.md). Пути к PSDK, sysroot, NDK и рабочему каталогу задаются переменными окружения; абсолютные пути конкретного пользователя в рецептах не требуются.

## Использованные проекты

Предварительный список; состав и лицензии компонентов runtime ещё требуют проверки перед публикацией.

- [Megler](https://github.com/Computershik73/megler-w10m) — основа проекта; автор — Илья Высоцкий.
- [Husk](https://github.com/Leviidev/Husk) — основа части Java framework; см. [уведомление](core/framework/NOTICE).
- [Android Open Source Project](https://source.android.com/) — компоненты Android runtime, включая libcore, Conscrypt, BoringSSL, ICU и Expat.
- Android runtime также содержит компоненты [OpenJDK](https://openjdk.org/), [Apache Harmony](https://harmony.apache.org/), [Apache Xalan](https://xalan.apache.org/), [Apache HttpComponents](https://hc.apache.org/), [Bouncy Castle](https://www.bouncycastle.org/), [OkHttp](https://github.com/square/okhttp) и [Netty](https://netty.io/).
- [zlib](https://zlib.net/) и [SQLite](https://sqlite.org/) — сжатие и база данных.
- [stb_vorbis](https://github.com/nothings/stb) и [minimp3](https://github.com/lieff/minimp3) — декодирование аудио; лицензия minimp3 находится [в репозитории](aurora/audio_minimp3_LICENSE.txt).
- [Qt](https://www.qt.io/) — интерфейс и системные мосты Авроры.
- Корневые сертификаты основаны на хранилище [Mozilla](https://www.mozilla.org/) в виде набора [curl](https://curl.se/ca/cacert.pem); подробности — в `core/cacerts/README.md`.

Aurora Store и microG — отдельные проекты, которые можно установить через настройки менеджера; они не являются частью исходного кода порта. Runtime также использует файлы из образа Android 9 для эмулятора Android SDK. Условия их распространения и необходимые уведомления ещё проверяются; список ссылок не заменяет лицензионные уведомления.

Инструкции по сборке и подробные ограничения: [aurora/README.md](aurora/README.md). Оригинальный README Megler сохранён в [README.upstream.md](README.upstream.md).
