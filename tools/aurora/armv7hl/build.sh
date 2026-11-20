#!/usr/bin/env bash
# Reproducible Aurora OS armv7hl build without sb2, chroot or sudo.
set -euo pipefail

RECIPE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$(cd "$RECIPE/../../.." && pwd)"
WORK="${WORKDIR:-$SRC/build/aurora-armv7hl}"
STEP="${1:-all}"
JOBS="${JOBS:-$(nproc)}"

pick_latest() {
    local directory="$1" pattern="$2"
    find "$directory" -mindepth 1 -maxdepth 1 -type d -name "$pattern" 2>/dev/null | sort -V | tail -1
}

if [[ -z "${PSDK:-}" ]]; then
    PSDK="$(pick_latest "$HOME" 'AuroraPlatformSDK-*')"
fi
: "${PSDK:?Set PSDK to an Aurora Platform SDK directory}"
if [[ -z "${SYS:-}" ]]; then
    SYS="$(pick_latest "$PSDK/targets" '*-base-armv7hl.default')"
fi
: "${SYS:?Set SYS to an Aurora armv7hl .default target}"
NDK="${MEGLER_NDK:-$WORK/android-ndk-r21e}"

need() { command -v "$1" >/dev/null || { echo "missing host tool: $1" >&2; exit 1; }; }
for tool in cmake make curl unzip python3 arm-linux-gnueabihf-gcc arm-linux-gnueabihf-g++ git rpmbuild rpm2cpio cpio; do
    need "$tool"
done
[[ -d "$SYS/usr/include/qt5/QtCore" ]] || { echo "invalid Aurora armv7hl sysroot: $SYS" >&2; exit 1; }

prepare_hosttools() {
    local host_target="${AURORA_HOST_TARGET:-$(pick_latest "$PSDK/targets" '*-base-x86_64')}"
    [[ -d "$host_target" ]] || { echo "no Aurora x86_64 target for Qt host tools" >&2; exit 1; }
    mkdir -p "$WORK/hosttools/bin" "$WORK/hosttools/lib"
    for library in libappdir-cpp.so.2 libappdir.so.2 libappmanifest-glib.so.1; do
        [[ -e "$WORK/hosttools/lib/$library" ]] || ln -sf "$host_target/usr/lib/$library" "$WORK/hosttools/lib/$library"
    done
    for tool in moc rcc uic lrelease lupdate; do
        printf '#!/bin/sh\nLD_LIBRARY_PATH="%s${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" exec "%s/usr/lib/qt5/bin/%s" "$@"\n' \
            "$WORK/hosttools/lib" "$host_target" "$tool" > "$WORK/hosttools/bin/$tool"
        chmod +x "$WORK/hosttools/bin/$tool"
    done
    "$WORK/hosttools/bin/moc" -v
}

fetch_sqlite() {
    (cd "$SRC" && bash tools/sqlite-fetch.sh)
    mkdir -p "$SRC/aurora/sqlite"
    cp -f "$SRC/build/sqlite/sqlite3.c" "$SRC/build/sqlite/sqlite3.h" "$SRC/aurora/sqlite/"
}

prepare_runtime() {
    local runtime="$WORK/android-runtime"
    if [[ ! -f "$WORK/upstream-0.2.10-aarch64.rpm" ]]; then
        mkdir -p "$WORK"
        curl -fL -o "$WORK/upstream-0.2.10-aarch64.rpm" \
            https://github.com/Arpolend2/AndroidCompatibilityLayer/releases/download/0.2.10/ru.androidcompatibilitylayer.appmanager-0.2.10-1.aarch64.rpm
    fi
    if [[ ! -f "$runtime/framework/megler-framework.dex" ]]; then
        rm -rf "$WORK/upstream-x" "$runtime"
        mkdir -p "$WORK/upstream-x" "$runtime"
        (cd "$WORK/upstream-x" && rpm2cpio "$WORK/upstream-0.2.10-aarch64.rpm" | cpio -idm --quiet \
            './usr/share/ru.androidcompatibilitylayer.appmanager/android-runtime/*')
        cp -a "$WORK/upstream-x/usr/share/ru.androidcompatibilitylayer.appmanager/android-runtime/." "$runtime/"
    fi
    if [[ ! -d "$SRC/build/android9/aosp/libcore" ]]; then
        (cd "$SRC" && bash tools/android9-sources.sh)
    fi
    if [[ ! -x "$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" ]]; then
        echo "Set MEGLER_NDK to Android NDK r21e (linux-x86_64)." >&2
        echo "Download: https://dl.google.com/android/repository/android-ndk-r21e-linux-x86_64.zip" >&2
        exit 1
    fi
    (cd "$SRC" && MEGLER_NDK="$NDK" MEGLER_NDK_HOST=linux-x86_64 MEGLER_JOBS="$JOBS" \
        bash tools/android9-natives.sh armeabi-v7a)
    mkdir -p "$runtime/lib"
    cp -f "$SRC"/build/android9/lib/*.so "$runtime/lib/"
    file "$runtime/lib/libjavacore.so"
}

build_app() {
    export PKG_CONFIG_SYSROOT_DIR="$SYS"
    export PKG_CONFIG_LIBDIR="$SYS/usr/lib/pkgconfig:$SYS/usr/share/pkgconfig"
    unset PKG_CONFIG_PATH
    rm -rf "$WORK/build"
    cmake -S "$SRC" -B "$WORK/build" \
        -DCMAKE_TOOLCHAIN_FILE="$RECIPE/toolchain.cmake" \
        -DAURORA_SYSROOT="$SYS" \
        -DAURORA_HOST_TOOLS="$WORK/hosttools" \
        -DANDROID_ROOT="$WORK/android-runtime" \
        -DCMAKE_BUILD_TYPE=Release \
        -DANDROID_COMPATIBILITY_LAYER_BUILD_TEST_TARGETS=OFF \
        -DANDROID_COMPATIBILITY_LAYER_EXPERIMENTAL_NATIVE_EGL=ON \
        -DANDROID_COMPATIBILITY_LAYER_EXPERIMENTAL_GPU_COMPOSITION=ON \
        -DANDROID_COMPATIBILITY_LAYER_EXPERIMENTAL_GPU_CANVAS=ON
    cmake --build "$WORK/build" -j"$JOBS"
    rm -rf "$WORK/stage"
    DESTDIR="$WORK/stage" cmake --install "$WORK/build" --prefix /usr >/dev/null
    arm-linux-gnueabihf-strip --strip-unneeded "$WORK/stage/usr/bin/ru.androidcompatibilitylayer.appmanager"
    file "$WORK/stage/usr/bin/ru.androidcompatibilitylayer.appmanager"
}

build_rpm() {
    mkdir -p "$WORK/rpmbuild"/{BUILD,RPMS,SOURCES,SPECS,SRPMS,tmp}
    rm -f "$WORK/rpmbuild/RPMS/armv7hl/"*.rpm
    rpmbuild -bb --target armv7hl \
        --define "_topdir $WORK/rpmbuild" --define "_tmppath $WORK/rpmbuild/tmp" \
        --define "acl_stage $WORK/stage" "$RECIPE/packaging.spec" \
        > "$WORK/rpmbuild.log" 2>&1 || { tail -30 "$WORK/rpmbuild.log" >&2; exit 1; }
    ls -la "$WORK/rpmbuild/RPMS/armv7hl/"
}

case "$STEP" in
    all) prepare_hosttools; fetch_sqlite; prepare_runtime; build_app; build_rpm ;;
    natives) prepare_runtime ;;
    app) prepare_hosttools; fetch_sqlite; prepare_runtime; build_app ;;
    rpm) build_rpm ;;
    *) echo "usage: $0 [all|natives|app|rpm]" >&2; exit 2 ;;
esac
