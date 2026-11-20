#!/bin/sh
set -eu

build_dir=$(cd "${1:?Usage: manager-child-launch-test.sh BUILD_DIR}" && pwd)
root=$(mktemp -d "$build_dir/manager-launch.XXXXXX")
trap 'rm -rf "$root"' EXIT HUP INT TERM

# Model private-bin: only the original manager filename is available.
manager="$root/ru.androidcompatibilitylayer.appmanager"
cp "$build_dir/android_compatibility_layer_preview_sessions_test" "$manager"
"$manager"
cp "$build_dir/android_compatibility_layer_probe_multitask_test" "$manager"
export QT_QPA_PLATFORM=minimal
"$manager"
"$manager" --rpm-guard-test
"$manager" --rpm-progress-test
"$manager" --rpm-cache-installer-test
