#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/acl-shared-microg.XXXXXX")
cleanup() {
    rm -f -- "$tmp/test" "$tmp/activated" "$tmp/activate.sh" \
        "$tmp/data/dbus-1/services/ru.androidcompatibilitylayer.appmanager.service"
    rmdir "$tmp/data/dbus-1/services" "$tmp/data/dbus-1" "$tmp/data" "$tmp"
}
trap cleanup EXIT
mkdir -p "$tmp/data/dbus-1/services"
printf '#!/bin/sh\nprintf activated > "%s/activated"\n' "$tmp" > "$tmp/activate.sh"
printf '[D-BUS Service]\nName=ru.androidcompatibilitylayer.appmanager\nExec=/bin/sh %s/activate.sh\n' "$tmp" \
    > "$tmp/data/dbus-1/services/ru.androidcompatibilitylayer.appmanager.service"

${CXX:-g++} -std=c++11 -fPIC \
    "$root/tools/shared-microg-ipc-test.cpp" "$root/aurora/shared_microg_ipc.cpp" \
    -o "$tmp/test" $(pkg-config --cflags --libs Qt5Core Qt5DBus)
XDG_DATA_HOME="$tmp/data" ACL_AUTOSTART_MARKER="$tmp/activated" dbus-run-session "$tmp/test"
