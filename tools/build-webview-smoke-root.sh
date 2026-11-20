#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APK="$ROOT/build/webview/arm64-v8a/webview.apk"
SOURCE_ROOT="$ROOT/build/android9/root-arm64-v8a"
OUT="$ROOT/build/webview-smoke/runtime-arm64-v8a"
BLOB=258dc814a9dc02ce953889440255d6787fca7b57

[ -f "$APK" ] || {
  echo "no AOSP Chromium 128 test provider; run tools/webview-fetch.sh --aosp-chromium-128-test-only arm64-v8a" >&2
  exit 1
}
[ "$(git -C "$ROOT" hash-object "$APK")" = "$BLOB" ] || {
  echo "AOSP Chromium 128 test provider failed its pinned Git blob check" >&2
  exit 1
}
[ -f "$SOURCE_ROOT/framework/megler-framework.dex" ] &&
  [ -f "$SOURCE_ROOT/framework/megler-libcore.dex" ] &&
  [ -f "$SOURCE_ROOT/lib/libjavacore.so" ] || {
    echo "no prepared ARM64 Android 9 root at $SOURCE_ROOT" >&2
    exit 1
  }

mkdir -p "$OUT"
cp -ru "$SOURCE_ROOT/." "$OUT/"
mkdir -p "$OUT/app/webview"
cp "$APK" "$OUT/app/webview/webview.apk"
printf '%s\n' "The platform support of the WebView: its natives are answered by the core (core/src/drivers/mg_java_webview.c)." \
  > "$OUT/lib/libwebviewchromium_plat_support.so"

[ -f "$OUT/app/webview/webview.apk" ] && cmp -s "$APK" "$OUT/app/webview/webview.apk" || {
  echo "the isolated WebView smoke runtime does not contain the pinned provider" >&2
  exit 1
}
echo "Built test-only Android root: $OUT"
