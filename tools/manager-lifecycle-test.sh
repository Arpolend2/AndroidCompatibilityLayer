#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
    echo "Usage: sh tools/manager-lifecycle-test.sh <manager-lifecycle-test-binary>" >&2
    exit 2
fi
tmp=$(mktemp -d "${TMPDIR:-/tmp}/acl-manager-lifecycle.XXXXXX")
cleanup() {
    rm -f -- "$tmp/lifecycle-test" "$tmp/events" \
        "$tmp/data/dbus-1/services/ru.androidcompatibilitylayer.appmanager.service" \
        "$tmp/config/ru.androidcompatibilitylayer/appmanager.conf"
    rmdir "$tmp/data/dbus-1/services" "$tmp/data/dbus-1" "$tmp/data" \
        "$tmp/config/ru.androidcompatibilitylayer" "$tmp/config" "$tmp"
}
trap cleanup EXIT
mkdir -p "$tmp/data/dbus-1/services" "$tmp/config/ru.androidcompatibilitylayer"
cp "$1" "$tmp/lifecycle-test"
printf '[D-BUS Service]\nName=ru.androidcompatibilitylayer.appmanager\nExec=%s/lifecycle-test --server %s/events auto\n' \
    "$tmp" "$tmp" > "$tmp/data/dbus-1/services/ru.androidcompatibilitylayer.appmanager.service"
XDG_DATA_HOME="$tmp/data" XDG_CONFIG_HOME="$tmp/config" \
    dbus-run-session "$tmp/lifecycle-test" --activation-client "$tmp/events"
