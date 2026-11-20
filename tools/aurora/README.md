# Кросс-сборка ACL для Aurora OS ARM

Эти рецепты собирают менеджер без `sb2`, chroot и `sudo`: кросс-компилятор Ubuntu
напрямую использует rootfs таргета Aurora Platform SDK. Хостовые Qt tools берутся из
x86_64-таргета того же PSDK, поэтому ARM-бинарники `moc`/`rcc` запускать не нужно.

## Требования

- Aurora Platform SDK с таргетами `*-base-<архитектура>.default` и `*-base-x86_64`;
- CMake, Make, Python 3, Git, curl, RPM tools, `rpm2cpio`, `cpio`;
- кросс-компиляторы:
  - `arm-linux-gnueabihf-gcc/g++` для ARMv7;
  - `aarch64-linux-gnu-gcc/g++` для AArch64;
- Android NDK r21e только для сборки ARMv7 Android runtime.

На Ubuntu основные пакеты можно установить так:

```bash
sudo apt install cmake make git curl unzip python3 file rpm rpm2cpio cpio \
  gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf \
  gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
```

`sudo` нужен только для установки зависимостей ОС, сама сборка его не использует.

## Команды

Из корня репозитория:

```bash
# ARMv7, включая сборку Android 9 native libraries из AOSP
bash tools/aurora/armv7hl/build.sh all

# AArch64; готовый Android 9 runtime берётся из upstream 0.2.10
bash tools/aurora/aarch64/build.sh all
```

По умолчанию скрипт находит последний `AuroraPlatformSDK-*` в `$HOME`, подходящий
`.default` sysroot и x86_64 target. Всё generated содержимое создаётся в игнорируемом
каталоге `build/aurora-<архитектура>/`.

Результаты:

```text
build/aurora-armv7hl/rpmbuild/RPMS/armv7hl/
build/aurora-aarch64/rpmbuild/RPMS/aarch64/
```

## Переопределение путей

Никаких путей конкретного пользователя в рецептах нет. Можно задать:

| Переменная | Назначение |
|---|---|
| `PSDK` | Каталог Aurora Platform SDK |
| `SYS` | Конкретный target rootfs |
| `WORKDIR` | Отдельный каталог generated-файлов |
| `AURORA_HOST_TARGET` | x86_64 target с Qt tools |
| `MEGLER_NDK` | Android NDK r21e для ARMv7 |
| `JOBS` | Число параллельных задач |

Пример:

```bash
PSDK=/opt/AuroraPlatformSDK \
SYS=/opt/AuroraPlatformSDK/targets/AuroraOS-base-aarch64.default \
WORKDIR=/tmp/acl-aarch64 \
JOBS=8 bash tools/aurora/aarch64/build.sh all
```

Поддерживаются отдельные этапы:

```bash
bash tools/aurora/armv7hl/build.sh natives
bash tools/aurora/armv7hl/build.sh app
bash tools/aurora/armv7hl/build.sh rpm

bash tools/aurora/aarch64/build.sh runtime
bash tools/aurora/aarch64/build.sh app
bash tools/aurora/aarch64/build.sh rpm
```

`ANDROID_ROOT` в CMake позволяет использовать подготовленный runtime вне исходного
дерева. Включены experimental native EGL, GPU composition и GPU Canvas. Для ARMv7
standalone действует CPU fallback, устраняющий конфликт `EGL_BAD_ACCESS` с Qt.

## Подпись RPM

Ниже используется публичный тестовый regular-ключ Aurora CLI. Не используйте его как
производственную подпись.

```bash
SDK_TOOLS="$PSDK/sdks/aurora_psdk"
RPM=/absolute/path/to/package.rpm

LD_LIBRARY_PATH=/tmp/rpmsignlibs \
OPENSSL_ENGINES="$SDK_TOOLS/usr/lib64/engines-1.1" \
  "$SDK_TOOLS/usr/bin/rpmsign-external" sign --force \
  --key="$HOME/.aurora-cli/keys/regular_key.pem" \
  --cert="$HOME/.aurora-cli/keys/regular_cert.pem" "$RPM"
```

`OPENSSL_ENGINES` обязателен для чтения ГОСТ-ключа. Каталог `/tmp/rpmsignlibs` должен
содержать библиотеки, необходимые конкретной версии `rpmsign-external`.

## Установка на устройство

```bash
DEVICE=192.168.1.110
scp "$RPM" "defaultuser@$DEVICE:/tmp/acl.rpm"
ssh "defaultuser@$DEVICE" \
  'busctl --system call ru.omp.APM /ru/omp/APM ru.omp.APM Install "sa{sv}" /tmp/acl.rpm 0'
```

Выбирайте пакет по **userspace**, а не только по `uname -m`: устройство с AArch64-ядром
может иметь 32-битный `armv7hl` userspace. Проверка:

```bash
getconf LONG_BIT
rpm -E %_arch
```

Подробности runtime: [ARMv7](armv7hl/README.md), [AArch64](aarch64/README.md).
