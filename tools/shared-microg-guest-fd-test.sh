#!/bin/sh
set -eu

: "${WORK_DIR:?Set WORK_DIR to an empty test output directory}"
mkdir -p "$WORK_DIR"
test -z "$(ls -A "$WORK_DIR")"
export WORK_DIR
CFLAGS="-std=gnu11 -O1 -D_GNU_SOURCE -ffunction-sections -fdata-sections -Icore/third_party/zlib"
export CFLAGS
find core/src -maxdepth 3 -name '*.c' -print0 |
    xargs -0 -P 8 -I@ sh -c 'gcc $CFLAGS -c "$1" -o "$WORK_DIR/$(echo "$1" | tr / _).o"' _ @
for source in core/third_party/zlib/*.c; do
    case "$source" in */gz*) continue;; esac
    gcc $CFLAGS -c "$source" -o "$WORK_DIR/$(basename "$source").o"
done
g++ -std=c++11 -fPIC -DACL_TEST_GUEST_FD $(pkg-config --cflags Qt5Core Qt5DBus) \
    -c tools/shared-microg-ipc-test.cpp -o "$WORK_DIR/fixture.o"
g++ -std=c++11 -fPIC $(pkg-config --cflags Qt5Core Qt5DBus) \
    -c aurora/shared_microg_ipc.cpp -o "$WORK_DIR/ipc.o"
g++ -Wl,--gc-sections -o "$WORK_DIR/guest-fd-test" "$WORK_DIR"/*.o \
    $(pkg-config --libs Qt5Core Qt5DBus) -lm -lpthread -ldl
mkdir -p "$WORK_DIR/data/dbus-1/services"
printf '#!/bin/sh\nprintf activated > "%s/activated"\n' "$WORK_DIR" > "$WORK_DIR/activate.sh"
printf '[D-BUS Service]\nName=ru.androidcompatibilitylayer.appmanager\nExec=/bin/sh %s/activate.sh\n' "$WORK_DIR" \
    > "$WORK_DIR/data/dbus-1/services/ru.androidcompatibilitylayer.appmanager.service"
XDG_DATA_HOME="$WORK_DIR/data" ACL_AUTOSTART_MARKER="$WORK_DIR/activated" \
    dbus-run-session -- "$WORK_DIR/guest-fd-test"
