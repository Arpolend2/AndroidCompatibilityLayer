#!/bin/sh
set -eu

: "${JAVA:?Set JAVA to the Linux JVM executable}"
: "${CLASSPATH:?Set CLASSPATH to the compiled test classes and Android API jar}"
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/acl-binder-ipc.XXXXXX")
cleanup() {
    rm -f -- "$tmp/test" "$tmp/worker-control-test"
    rmdir "$tmp"
}
trap cleanup EXIT

${CC:-gcc} -std=gnu11 -ffunction-sections -fdata-sections \
    "$root/core/tests/microg-binder-boundary/worker_control_test.c" \
    -Wl,--gc-sections -pthread -o "$tmp/worker-control-test"
"$tmp/worker-control-test"

${CXX:-g++} -std=c++11 -fPIC \
    "$root/tools/shared-microg-binder-test.cpp" "$root/aurora/shared_microg_ipc.cpp" \
    -o "$tmp/test" $(pkg-config --cflags --libs Qt5Core Qt5DBus)
dbus-run-session "$tmp/test"
dbus-run-session "$tmp/test" --drop
dbus-run-session "$tmp/test" --manager-error
dbus-run-session "$tmp/test" --manager-jvm
