# Aurora OS aarch64

```bash
bash tools/aurora/aarch64/build.sh all
```

Менеджер собирается из текущего дерева с `-march=armv8-a`. Подготовленный Android 9
AArch64 runtime извлекается из соответствующего upstream RPM 0.2.10, поэтому NDK и
повторная сборка AOSP для этой архитектуры не требуются.

Runtime размещается отдельно в `build/aurora-aarch64/android-runtime` и передаётся в
CMake через `ANDROID_ROOT`; ARMv7- и AArch64-библиотеки не смешиваются.

По умолчанию результат находится в:

```text
build/aurora-aarch64/rpmbuild/RPMS/aarch64/
  ru.androidcompatibilitylayer.appmanager-0.2.10-10.aarch64.rpm
```

Проверки:

```bash
file build/aurora-aarch64/stage/usr/bin/ru.androidcompatibilitylayer.appmanager
file build/aurora-aarch64/android-runtime/lib/libjavacore.so
rpm -qpi build/aurora-aarch64/rpmbuild/RPMS/aarch64/*.rpm
rpm -qpR build/aurora-aarch64/rpmbuild/RPMS/aarch64/*.rpm | grep 64bit
```

При наличии `qemu-aarch64-static` можно проверить запуск бинарника с sysroot (это не
заменяет тест GPU и Android-приложений на физическом устройстве):

```bash
QT_QPA_PLATFORM=minimal qemu-aarch64-static -L "$SYS" \
  build/aurora-aarch64/stage/usr/bin/ru.androidcompatibilitylayer.appmanager --gpu-probe
```

ARMv7-only CPU workaround для standalone здесь не применяется. Фактические пути
DMA-BUF/Hybris gralloc зависят от EGL-драйвера конкретного AArch64-устройства.
