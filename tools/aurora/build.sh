#!/usr/bin/env bash
set -euo pipefail

RECIPE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$(cd "$RECIPE/../.." && pwd)"
ARCH="${1:?Use the aarch64/build.sh or armv7hl/build.sh wrapper}"
STEP="${2:-check}"
case "$ARCH" in
    aarch64) TRIPLET=aarch64-meego-linux-gnu; DEFAULT_RUNTIME="$SRC/aurora/android-runtime" ;;
    armv7hl) TRIPLET=armv7hl-meego-linux-gnueabi; DEFAULT_RUNTIME="$SRC/build/android9/root-armeabi-v7a" ;;
    *) echo "Unsupported Aurora architecture: $ARCH" >&2; exit 2 ;;
esac
case "$STEP" in
    check|configure|build|stage) ;;
    *) echo "Usage: $0 $ARCH [check|configure|build|stage]" >&2; exit 2 ;;
esac
SYS="${SYS:-/opt/cross/$TRIPLET/sys-root}"
HOST_TOOLS="${AURORA_HOST_TOOLS:-/usr/lib64/qt5/bin}"
CROSS="${AURORA_CROSS_PREFIX:-/opt/cross/bin/$TRIPLET-}"
RUNTIME="${ANDROID_ROOT:-$DEFAULT_RUNTIME}"
WORK="${WORKDIR:-$SRC/build/aurora-$ARCH}"
if [[ "$STEP" == stage && -e "$WORK/stage" ]]; then
    echo "Stage already exists: $WORK/stage. Use a fresh WORKDIR to avoid packaging stale files." >&2
    exit 1
fi

for tool in python3 pkg-config cmake "${CROSS}gcc" "${CROSS}g++" "${CROSS}readelf"; do
    command -v "$tool" >/dev/null || { echo "Missing build tool: $tool. Set AURORA_CROSS_PREFIX for your SDK." >&2; exit 1; }
done
[[ -d "$SYS/usr" ]] || { echo "Missing target sysroot: $SYS. Set SYS to the matching Aurora SDK target." >&2; exit 1; }
SYS="$(cd "$SYS" && pwd)"
export PKG_CONFIG_SYSROOT_DIR="$SYS"
export PKG_CONFIG_LIBDIR="$SYS/usr/lib64/pkgconfig:$SYS/usr/lib/pkgconfig:$SYS/usr/share/pkgconfig"
unset PKG_CONFIG_PATH
python3 "$RECIPE/verify.py" sdk "$ARCH" "$SYS" --host-tools "$HOST_TOOLS" --cross-prefix "$CROSS"
python3 "$RECIPE/verify.py" runtime "$ARCH" "$RUNTIME"
if [[ "$STEP" == check ]]; then exit 0; fi

cmake -S "$SRC" -B "$WORK/build" \
    -DCMAKE_TOOLCHAIN_FILE="$RECIPE/$ARCH/toolchain.cmake" \
    -DAURORA_SYSROOT="$SYS" -DAURORA_HOST_TOOLS="$HOST_TOOLS" \
    -DAURORA_CROSS_PREFIX="$CROSS" -DANDROID_ROOT="$RUNTIME" \
    -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_SKIP_RPATH=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -DANDROID_COMPATIBILITY_LAYER_BUILD_TEST_TARGETS=OFF \
    -DANDROID_COMPATIBILITY_LAYER_EXPERIMENTAL_NATIVE_EGL=ON \
    -DANDROID_COMPATIBILITY_LAYER_EXPERIMENTAL_GPU_COMPOSITION=ON \
    -DANDROID_COMPATIBILITY_LAYER_EXPERIMENTAL_GPU_CANVAS=ON
if [[ "$STEP" == configure ]]; then exit 0; fi
cmake --build "$WORK/build" --parallel "${JOBS:-2}"
if [[ "$STEP" == build ]]; then exit 0; fi

DESTDIR="$WORK/stage" cmake --install "$WORK/build"
python3 "$RECIPE/verify.py" stage "$ARCH" "$WORK/stage/usr" \
    --abi-file "$SRC/aurora/standalone-runtime-abi" --readelf "${CROSS}readelf"
echo "Verified installation: $WORK/stage. RPM packaging remains in rpm/ru.androidcompatibilitylayer.appmanager.spec (Aurora IDE/SDK)."
