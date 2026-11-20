# Aurora OS armv7hl

```bash
bash tools/aurora/armv7hl/build.sh all
```

Скрипт выполняет четыре независимых этапа:

1. Загружает SQLite amalgamation через `tools/sqlite-fetch.sh`.
2. Извлекает архитектурно-нейтральные части Android 9 runtime из upstream RPM 0.2.10.
3. Получает AOSP `android-9.0.0_r47` через `tools/android9-sources.sh` и собирает
   `armeabi-v7a` native libraries через `tools/android9-natives.sh` и NDK r21e.
4. Собирает менеджер Qt/GLES и упаковывает его вместе с отдельным ARMv7 runtime.

NDK по умолчанию ищется в `build/aurora-armv7hl/android-ndk-r21e`; удобнее задать его
явно:

```bash
MEGLER_NDK=/opt/android-ndk-r21e bash tools/aurora/armv7hl/build.sh all
```

NDK r21e Linux x86_64:
https://dl.google.com/android/repository/android-ndk-r21e-linux-x86_64.zip

Параметры менеджера: ARMv7-A, Thumb-2, VFPv3-D16, hard-float. Runtime остаётся
`armeabi-v7a`, softfp — так его собирают существующие AOSP-скрипты проекта.
`libcrypto.so` и `libssl.so` берутся из AOSP VNDK 28 и используют NEON.

По умолчанию результат находится в:

```text
build/aurora-armv7hl/rpmbuild/RPMS/armv7hl/
  ru.androidcompatibilitylayer.appmanager-0.2.10-10.armv7hl.rpm
```

Проверить бинарники можно так:

```bash
file build/aurora-armv7hl/stage/usr/bin/ru.androidcompatibilitylayer.appmanager
file build/aurora-armv7hl/android-runtime/lib/libjavacore.so
rpm -qpi build/aurora-armv7hl/rpmbuild/RPMS/armv7hl/*.rpm
```

На ARMv7 standalone временно используется CPU Canvas: экспериментальный общий
Android/Qt EGL state вызывал `QWaylandGLContext: EGL_BAD_ACCESS` и чёрный экран.
Embedded worker по-прежнему пытается использовать DMA-BUF или Hybris gralloc,
сохраняя CPU fallback в соответствии с возможностями устройства. `--gpu-probe` показывает доступные
EGL extensions, ION и результат тестовой gralloc-аллокации.
