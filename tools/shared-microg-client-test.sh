#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/acl-shared-microg-client.XXXXXX")
cleanup() {
    rm -f -- "$tmp/test"
    rmdir "$tmp"
}
trap cleanup EXIT

${CXX:-g++} -std=c++11 -fPIC \
    "$root/tools/shared-microg-binder-test.cpp" "$root/aurora/shared_microg_ipc.cpp" \
    -o "$tmp/test" $(pkg-config --cflags --libs Qt5Core Qt5DBus)
dbus-run-session -- "$tmp/test" --manager-error
dbus-run-session -- "$tmp/test" --manager-smoke
dbus-run-session -- "$tmp/test" --manager-cancel
dbus-run-session -- "$tmp/test" --manager-auth
dbus-run-session -- "$tmp/test" --manager-metadata
dbus-run-session -- "$tmp/test" --manager-legacy
dbus-run-session -- "$tmp/test" --manager-untrusted
dbus-run-session -- "$tmp/test" --manager-wrongpackage
