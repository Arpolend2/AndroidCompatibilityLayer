#!/usr/bin/env bash
# Reproducible Aurora OS aarch64 build without sb2, chroot or sudo.
set -euo pipefail

RECIPE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$(cd "$RECIPE/../../.." && pwd)"
WORK="${WORKDIR:-$SRC/build/aurora-aarch64}"
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
    SYS="$(pick_latest "$PSDK/targets" '*-base-aarch64.default')"
fi
: "${SYS:?Set SYS to an Aurora aarch64 .default target}"

need() { command -v "$1" >/dev/null || { echo "missing host tool: $1" >&2; exit 1; }; }
for tool in cmake make curl python3 aarch64-linux-gnu-gcc aarch64-linux-gnu-g++ git rpmbuild rpm2cpio cpio; do
    need "$tool"
done
[[ -d "$SYS/usr/include/qt5/QtCore" ]] || { echo "invalid Aurora aarch64 sysroot: $SYS" >&2; exit 1; }

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
    if [[ ! -f "$runtime/lib/libjavacore.so" ]]; then
        rm -rf "$WORK/upstream-x" "$runtime"
        mkdir -p "$WORK/upstream-x" "$runtime"
        (cd "$WORK/upstream-x" && rpm2cpio "$WORK/upstream-0.2.10-aarch64.rpm" | cpio -idm --quiet \
            './usr/share/ru.androidcompatibilitylayer.appmanager/android-runtime/*')
        cp -a "$WORK/upstream-x/usr/share/ru.androidcompatibilitylayer.appmanager/android-runtime/." "$runtime/"
    fi
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
    aarch64-linux-gnu-strip --strip-unneeded "$WORK/stage/usr/bin/ru.androidcompatibilitylayer.appmanager"
    file "$WORK/stage/usr/bin/ru.androidcompatibilitylayer.appmanager"
}

build_rpm() {
    mkdir -p "$WORK/rpmbuild"/{BUILD,RPMS,SOURCES,SPECS,SRPMS,tmp}
    rm -f "$WORK/rpmbuild/RPMS/aarch64/"*.rpm
    rpmbuild -bb --target aarch64 \
        --define "_topdir $WORK/rpmbuild" --define "_tmppath $WORK/rpmbuild/tmp" \
        --define "acl_stage $WORK/stage" "$RECIPE/packaging.spec" \
        > "$WORK/rpmbuild.log" 2>&1 || { tail -30 "$WORK/rpmbuild.log" >&2; exit 1; }
    ls -la "$WORK/rpmbuild/RPMS/aarch64/"
}

case "$STEP" in
    all) prepare_hosttools; fetch_sqlite; prepare_runtime; build_app; build_rpm ;;
    runtime) prepare_runtime ;;
    app) prepare_hosttools; fetch_sqlite; prepare_runtime; build_app ;;
    rpm) build_rpm ;;
    *) echo "usage: $0 [all|runtime|app|rpm]" >&2; exit 2 ;;
esac
