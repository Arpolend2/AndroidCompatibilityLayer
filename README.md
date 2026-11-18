# AndroidCompatibilityLayer

**Порт проекта [Megler](https://github.com/Computershik73/megler-w10m) под ОС Аврора. Автор оригинального проекта — [Илья Высоцкий (Computershik73)](https://github.com/Computershik73).**

Добавляйте Android-приложения в библиотеку, настраивайте разрешения и создавайте самостоятельные RPM.

Проект экспериментальный: совместимость не гарантируется. Подписи APK не проверяются — устанавливайте только доверенные файлы.

Лицензия: [GPL-3.0](LICENSE).

## Реализовано

- Импорт APK, APKS и XAPK.
- Создание самостоятельных RPM и настройка разрешений.
- Экспериментальные мосты камеры, датчиков, Bluetooth LE и вывода звука.

## Пока не работает или ограничено

- Android WebView и воспроизведение видео.
- GPU-ускорение находится в экспериментальном состоянии и по умолчанию отключено.

## Использованные проекты

Предварительный список; состав и лицензии компонентов runtime ещё требуют проверки перед публикацией.

- [Megler](https://github.com/Computershik73/megler-w10m) — основа проекта; автор — Илья Высоцкий.
- [Husk](https://github.com/Leviidev/Husk) — основа части Java framework; см. `core/framework/NOTICE`.
- [Android Open Source Project](https://source.android.com/) — компоненты Android runtime, включая libcore, Conscrypt, BoringSSL, ICU и Expat.
- Android runtime также содержит компоненты [OpenJDK](https://openjdk.org/), [Apache Harmony](https://harmony.apache.org/), [Apache Xalan](https://xalan.apache.org/), [Apache HttpComponents](https://hc.apache.org/), [Bouncy Castle](https://www.bouncycastle.org/), [OkHttp](https://github.com/square/okhttp) и [Netty](https://netty.io/).
- [zlib](https://zlib.net/) и [SQLite](https://sqlite.org/) — сжатие и база данных.
- [stb_vorbis](https://github.com/nothings/stb) и [minimp3](https://github.com/lieff/minimp3) — декодирование аудио.
- Корневые сертификаты основаны на хранилище [Mozilla](https://www.mozilla.org/) в виде набора [curl](https://curl.se/ca/cacert.pem); подробности — в `core/cacerts/README.md`.

Runtime также использует файлы из образа Android 9 для эмулятора Android SDK. Условия их распространения и необходимые уведомления ещё проверяются.

Инструкции по сборке и подробные ограничения: [aurora/README.md](aurora/README.md). Оригинальный README Megler сохранён в [README.upstream.md](README.upstream.md).
