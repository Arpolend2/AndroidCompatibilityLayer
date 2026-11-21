#!/bin/sh
set -eu
: "${WORK_DIR:?Set WORK_DIR to an empty output directory}"
: "${GUEST_APK:?Set GUEST_APK}"
: "${GUEST_ANDROID_ROOT:?Set GUEST_ANDROID_ROOT}"
: "${FRAMEWORK_DEX:?Set FRAMEWORK_DEX}"
: "${JAVA:?Set JAVA}"
: "${CLASSPATH:?Set CLASSPATH}"
: "${RESULT_DIR:?Set RESULT_DIR}"
mkdir -p "$WORK_DIR"
test -z "$(ls -A "$WORK_DIR")"
export WORK_DIR
CFLAGS="-std=gnu11 -O1 -D_GNU_SOURCE -ffunction-sections -fdata-sections -Icore/third_party/zlib"
if [ -f build/sqlite/sqlite3.c ] && [ -f build/sqlite/sqlite3.h ]; then
    CFLAGS="$CFLAGS -DMG_HAVE_SQLITE=1 -Ibuild/sqlite"
elif [ -n "${REAL_GMS_APK:-}" ]; then
    echo "Real microG test requires build/sqlite/sqlite3.c and sqlite3.h" >&2
    exit 1
fi
export CFLAGS
{
    find core/src -maxdepth 3 -name '*.c'
    find core/tests -maxdepth 1 -name '*.c'
} | xargs -P 8 -I@ sh -c 'gcc $CFLAGS -c "$1" -o "$WORK_DIR/$(echo "$1" | tr / _).o"' _ @
for source in core/third_party/zlib/*.c; do
    case "$source" in */gz*) continue;; esac
    gcc $CFLAGS -c "$source" -o "$WORK_DIR/$(basename "$source").o"
done
gcc -Wl,--gc-sections -o "$WORK_DIR/mg_tests" "$WORK_DIR"/*.o -lm -lpthread -ldl
g++ -std=c++11 -fPIC tools/shared-microg-binder-test.cpp aurora/shared_microg_ipc.cpp \
    -o "$WORK_DIR/fixture" $(pkg-config --cflags --libs Qt5Core Qt5DBus)
cp -a "$GUEST_ANDROID_ROOT" "$WORK_DIR/android-root"
cp "$FRAMEWORK_DEX/classes.dex" "$WORK_DIR/android-root/framework/megler-framework.dex"
cp "$FRAMEWORK_DEX/classes2.dex" "$WORK_DIR/android-root/framework/megler-framework-classes2.dex"
mkdir "$WORK_DIR/app-data"
mkdir "$WORK_DIR/server-data"
export GUEST_APK JAVA CLASSPATH
GUEST_RUNNER="$WORK_DIR/mg_tests"
GUEST_DATA="$WORK_DIR/app-data"
GUEST_SERVER_DATA="$WORK_DIR/server-data"
GUEST_ANDROID_ROOT="$WORK_DIR/android-root"
GUEST_PICTURES="$RESULT_DIR/pictures"
GUEST_SERVER_PICTURES="$RESULT_DIR/server-pictures"
export GUEST_RUNNER GUEST_DATA GUEST_SERVER_DATA GUEST_ANDROID_ROOT GUEST_PICTURES GUEST_SERVER_PICTURES
if [ -z "${SELF_HOSTED_ONLY:-}" ]; then
if ! dbus-run-session -- "$WORK_DIR/fixture" --guest > "$RESULT_DIR/guest-binder.log" 2>&1; then
    tail -n 100 "$RESULT_DIR/guest-binder.log"
    exit 1
fi
if ! grep -q 'BinderGuestTest: PASS transaction, nested callback, one-way and death' "$RESULT_DIR/guest-binder.log" ||
   grep -Eq 'BinderGuestTest: FAIL|Binder transport shutdown failed|run: the app has ended \(status -' "$RESULT_DIR/guest-binder.log"; then
    tail -n 100 "$RESULT_DIR/guest-binder.log"
    exit 1
fi
grep 'BinderGuestTest: PASS' "$RESULT_DIR/guest-binder.log"
mkdir "$WORK_DIR/app-data-two" "$WORK_DIR/server-data-two"
GUEST_DATA="$WORK_DIR/app-data-two"
GUEST_SERVER_DATA="$WORK_DIR/server-data-two"
GUEST_PICTURES="$RESULT_DIR/two-client-pictures"
GUEST_SERVER_PICTURES="$RESULT_DIR/two-server-pictures"
export GUEST_DATA GUEST_SERVER_DATA GUEST_PICTURES GUEST_SERVER_PICTURES
if ! dbus-run-session -- "$WORK_DIR/fixture" --two-guests > "$RESULT_DIR/two-guests.log" 2>&1; then
    tail -n 100 "$RESULT_DIR/two-guests.log"
    exit 1
fi
if ! grep -q 'BinderGuestTest: PASS transaction, nested callback, one-way and death' "$RESULT_DIR/two-guests.log" ||
   ! grep -q 'BinderGuestTest: PASS guest server closed' "$RESULT_DIR/two-guests.log" ||
   grep -Eq 'BinderGuestTest: FAIL|Binder transport shutdown failed|run: the app has ended \(status -' "$RESULT_DIR/two-guests.log"; then
    tail -n 100 "$RESULT_DIR/two-guests.log"
    exit 1
fi
grep 'BinderGuestTest: PASS' "$RESULT_DIR/two-guests.log"
mkdir "$WORK_DIR/app-data-broker" "$WORK_DIR/server-data-broker"
GUEST_DATA="$WORK_DIR/app-data-broker"
GUEST_SERVER_DATA="$WORK_DIR/server-data-broker"
GUEST_PICTURES="$RESULT_DIR/broker-client-pictures"
GUEST_SERVER_PICTURES="$RESULT_DIR/broker-server-pictures"
GUEST_GMS_APK="$RESULT_DIR/broker.apk"
MEGLER_TEST_BINDER_BROKER=1
export GUEST_DATA GUEST_SERVER_DATA GUEST_PICTURES GUEST_SERVER_PICTURES GUEST_GMS_APK MEGLER_TEST_BINDER_BROKER
if ! dbus-run-session -- "$WORK_DIR/fixture" --two-guests > "$RESULT_DIR/broker-guests.log" 2>&1; then
    tail -n 100 "$RESULT_DIR/broker-guests.log"
    exit 1
fi
if ! grep -q 'BinderGuestTest: PASS cross-process bound service and callback' "$RESULT_DIR/broker-guests.log" ||
   ! grep -q 'BinderGuestTest: PASS dynamic binding lifecycle' "$RESULT_DIR/broker-guests.log" ||
   ! grep -q 'BinderGuestTest: PASS transaction, nested callback, one-way and death' "$RESULT_DIR/broker-guests.log" ||
   ! grep -q 'BinderGuestTest: PASS guest server closed' "$RESULT_DIR/broker-guests.log" ||
   grep -Eq 'BinderGuestTest: FAIL|Binder transport shutdown failed|run: the app has ended \(status -' "$RESULT_DIR/broker-guests.log"; then
    tail -n 100 "$RESULT_DIR/broker-guests.log"
    exit 1
fi
grep 'BinderGuestTest: PASS' "$RESULT_DIR/broker-guests.log"
fi
if [ -n "${REAL_GMS_APK:-}" ]; then
    test -f "$REAL_GMS_APK"
    gcc core/tests/microg-binder-boundary/sqlite-wal-fixture.c -o "$WORK_DIR/sqlite-wal-fixture" -lsqlite3
    GUEST_GMS_APK="$REAL_GMS_APK"
    MEGLER_TEST_BINDER_BROKER=real
    export GUEST_GMS_APK MEGLER_TEST_BINDER_BROKER
    if [ -z "${SELF_HOSTED_ONLY:-}" ]; then
    mkdir "$WORK_DIR/app-data-real" "$WORK_DIR/server-data-real"
    mkdir -p "$WORK_DIR/server-data-real/apps/megler.test.binderfd/data/tmp"
    "$WORK_DIR/sqlite-wal-fixture" "$WORK_DIR/server-data-real/apps/megler.test.binderfd/data/tmp/binder-wal.db"
    test -s "$WORK_DIR/server-data-real/apps/megler.test.binderfd/data/tmp/binder-wal.db-wal"
    GUEST_DATA="$WORK_DIR/app-data-real"
    GUEST_SERVER_DATA="$WORK_DIR/server-data-real"
    GUEST_PICTURES="$RESULT_DIR/real-client-pictures"
    GUEST_SERVER_PICTURES="$RESULT_DIR/real-server-pictures"
    GUEST_GMS_APK="$REAL_GMS_APK"
    MEGLER_TEST_BINDER_BROKER=real
    export GUEST_DATA GUEST_SERVER_DATA GUEST_PICTURES GUEST_SERVER_PICTURES GUEST_GMS_APK MEGLER_TEST_BINDER_BROKER
    if ! dbus-run-session -- "$WORK_DIR/fixture" --two-guests > "$RESULT_DIR/real-guests.log" 2>&1; then
        tail -n 100 "$RESULT_DIR/real-guests.log"
        exit 1
    fi
    if ! grep -q 'BinderGuestTest: PASS cross-process real microG service' "$RESULT_DIR/real-guests.log" ||
       ! grep -q 'BinderGuestTest: PASS dynamic binding lifecycle' "$RESULT_DIR/real-guests.log" ||
       ! grep -q 'BinderGuestTest: PASS guest SQLite paths and attach' "$RESULT_DIR/real-guests.log" ||
       ! grep -q 'BinderGuestTest: PASS guest SQLite WAL recovery' "$RESULT_DIR/real-guests.log" ||
       ! grep -q 'BinderGuestTest: PASS cross-process real microG broker and callback' "$RESULT_DIR/real-guests.log" ||
       ! grep -q 'BinderGuestTest: PASS transaction, nested callback, one-way and death' "$RESULT_DIR/real-guests.log" ||
       ! grep -q 'BinderGuestTest: PASS guest server closed' "$RESULT_DIR/real-guests.log" ||
       grep -Eq 'BinderGuestTest: FAIL|Binder transport shutdown failed|run: the app has ended \(status -' "$RESULT_DIR/real-guests.log"; then
        tail -n 100 "$RESULT_DIR/real-guests.log"
        exit 1
    fi
    grep 'BinderGuestTest: PASS' "$RESULT_DIR/real-guests.log"
    mkdir -p "$WORK_DIR/app-data-headless" "$WORK_DIR/server-data-headless/apps/megler.test.binderfd/data/tmp"
    "$WORK_DIR/sqlite-wal-fixture" "$WORK_DIR/server-data-headless/apps/megler.test.binderfd/data/tmp/binder-wal.db"
    test -s "$WORK_DIR/server-data-headless/apps/megler.test.binderfd/data/tmp/binder-wal.db-wal"
    GUEST_DATA="$WORK_DIR/app-data-headless"
    GUEST_SERVER_DATA="$WORK_DIR/server-data-headless"
    GUEST_PICTURES="$RESULT_DIR/headless-client-pictures"
    GUEST_SERVER_PICTURES="$RESULT_DIR/headless-server-pictures"
    GUEST_SERVER_APK="$RESULT_DIR/headless.apk"
    GUEST_HEADLESS_SERVER=1
    GUEST_HEADLESS_NO_DEX="$RESULT_DIR/headless-no-dex.apk"
    GUEST_HEADLESS_WRONG_ABI="$RESULT_DIR/headless-wrong-abi.apk"
    GUEST_HEADLESS_SPLIT="$RESULT_DIR/headless-split.apk"
    export GUEST_DATA GUEST_SERVER_DATA GUEST_PICTURES GUEST_SERVER_PICTURES GUEST_SERVER_APK GUEST_HEADLESS_SERVER \
           GUEST_HEADLESS_NO_DEX GUEST_HEADLESS_WRONG_ABI GUEST_HEADLESS_SPLIT
    if ! dbus-run-session -- "$WORK_DIR/fixture" --two-guests > "$RESULT_DIR/headless-guests.log" 2>&1; then
        tail -n 100 "$RESULT_DIR/headless-guests.log"
        exit 1
    fi
    if ! grep -q 'BinderGuestTest: PASS headless application started' "$RESULT_DIR/headless-guests.log" ||
       ! grep -q 'BinderGuestTest: PASS cross-process real microG service' "$RESULT_DIR/headless-guests.log" ||
       ! grep -q 'BinderGuestTest: PASS cross-process real microG broker and callback' "$RESULT_DIR/headless-guests.log" ||
       ! grep -q 'BinderGuestTest: PASS dynamic binding lifecycle' "$RESULT_DIR/headless-guests.log" ||
       ! grep -q 'PASS headless APK validation and ordinary launch refusal' "$RESULT_DIR/headless-guests.log" ||
       ! grep -q 'PASS headless runtime stopped without an activity' "$RESULT_DIR/headless-guests.log" ||
       grep -Eq 'BinderGuestTest: FAIL|Binder transport shutdown failed|run: the app has ended \(status -|FAIL:' "$RESULT_DIR/headless-guests.log"; then
        tail -n 100 "$RESULT_DIR/headless-guests.log"
        exit 1
    fi
    grep 'PASS headless\|BinderGuestTest: PASS' "$RESULT_DIR/headless-guests.log"
    mkdir -p "$WORK_DIR/app-data-activity" "$WORK_DIR/server-data-activity/apps/megler.test.binderfd/data/tmp"
    "$WORK_DIR/sqlite-wal-fixture" "$WORK_DIR/server-data-activity/apps/megler.test.binderfd/data/tmp/binder-wal.db"
    GUEST_DATA="$WORK_DIR/app-data-activity"
    GUEST_SERVER_DATA="$WORK_DIR/server-data-activity"
    GUEST_PICTURES="$RESULT_DIR/activity-client-pictures"
    GUEST_SERVER_PICTURES="$RESULT_DIR/activity-server-pictures"
    GUEST_SERVER_APK="$RESULT_DIR/headless-activity.apk"
    GUEST_HEADLESS_ACTIVITY=1
    export GUEST_DATA GUEST_SERVER_DATA GUEST_PICTURES GUEST_SERVER_PICTURES GUEST_SERVER_APK GUEST_HEADLESS_ACTIVITY
    if ! dbus-run-session -- "$WORK_DIR/fixture" --two-guests > "$RESULT_DIR/headless-activity.log" 2>&1; then
        tail -n 100 "$RESULT_DIR/headless-activity.log"
        exit 1
    fi
    if ! grep -q 'BinderGuestTest: PASS headless activity destroyed' "$RESULT_DIR/headless-activity.log" ||
       ! grep -q 'BinderGuestTest: PASS cross-process real microG broker and callback' "$RESULT_DIR/headless-activity.log" ||
       ! grep -q 'PASS headless runtime stopped without an activity' "$RESULT_DIR/headless-activity.log" ||
       grep -Eq 'BinderGuestTest: FAIL|Binder transport shutdown failed|run: the app has ended \(status -|FAIL:' "$RESULT_DIR/headless-activity.log"; then
        tail -n 100 "$RESULT_DIR/headless-activity.log"
        exit 1
    fi
    echo "Headless service survived closing its last Activity"
    mkdir "$WORK_DIR/real-headless-data" "$RESULT_DIR/real-headless-pictures"
    if ! "$GUEST_RUNNER" --java headless-real "$REAL_GMS_APK" "$WORK_DIR/real-headless-data" \
        "$GUEST_ANDROID_ROOT" "$RESULT_DIR/real-headless-pictures" 1 800x480@160 \
        > "$RESULT_DIR/real-headless.log" 2>&1; then
        tail -n 100 "$RESULT_DIR/real-headless.log"
        exit 1
    fi
    if ! grep -q 'I/Megler: headless application started' "$RESULT_DIR/real-headless.log" ||
       ! grep -q 'PASS headless runtime stopped without an activity' "$RESULT_DIR/real-headless.log"; then
        tail -n 100 "$RESULT_DIR/real-headless.log"
        exit 1
    fi
    echo "Real microG APK started and stopped headless"
    fi
    mkdir -p "$WORK_DIR/app-data-own-microg" "$WORK_DIR/server-data-own-microg/services/data-microg/com.google.android.gms/tmp"
    "$WORK_DIR/sqlite-wal-fixture" "$WORK_DIR/server-data-own-microg/services/data-microg/com.google.android.gms/tmp/binder-wal.db"
    GUEST_DATA="$WORK_DIR/app-data-own-microg"
    GUEST_SERVER_DATA="$WORK_DIR/server-data-own-microg"
    GUEST_PICTURES="$RESULT_DIR/own-microg-client-pictures"
    GUEST_SERVER_PICTURES="$RESULT_DIR/own-microg-server-pictures"
    GUEST_SERVER_APK="$RESULT_DIR/microg-test-server.apk"
    GUEST_HEADLESS_SERVER=1
    GUEST_HEADLESS_REAL_SERVER=1
    unset GUEST_HEADLESS_ACTIVITY
    export GUEST_DATA GUEST_SERVER_DATA GUEST_PICTURES GUEST_SERVER_PICTURES GUEST_SERVER_APK GUEST_HEADLESS_SERVER GUEST_HEADLESS_REAL_SERVER
    if ! dbus-run-session -- "$WORK_DIR/fixture" --two-guests > "$RESULT_DIR/own-microg-guests.log" 2>&1; then
        tail -n 120 "$RESULT_DIR/own-microg-guests.log"
        exit 1
    fi
    if ! grep -q 'I/Megler: headless application started' "$RESULT_DIR/own-microg-guests.log" ||
       ! grep -q 'BinderGuestTest: PASS cross-process real microG service' "$RESULT_DIR/own-microg-guests.log" ||
       ! grep -q 'BinderGuestTest: PASS cross-process real microG broker and callback' "$RESULT_DIR/own-microg-guests.log" ||
       ! grep -q 'BinderGuestTest: PASS self-hosted broker rejected another package' "$RESULT_DIR/own-microg-guests.log" ||
       ! grep -q 'Binder dispatch failed: java.lang.SecurityException: UID .* is not related to packageName \[com.example.other\]' "$RESULT_DIR/own-microg-guests.log" ||
       ! grep -q 'BinderGuestTest: PASS dynamic binding lifecycle' "$RESULT_DIR/own-microg-guests.log" ||
       ! grep -q 'PASS headless runtime stopped without an activity' "$RESULT_DIR/own-microg-guests.log" ||
       grep -Eq 'BinderGuestTest: FAIL|Binder transport shutdown failed|run: the app has ended \(status -|FAIL:' "$RESULT_DIR/own-microg-guests.log"; then
        tail -n 120 "$RESULT_DIR/own-microg-guests.log"
        exit 1
    fi
    echo "Real microG APK served Binder and broker with fixture-bound caller identity"
    mkdir -p "$WORK_DIR/app-data-manager-microg" "$WORK_DIR/server-data-manager-microg/services/data-microg/com.google.android.gms/tmp"
    "$WORK_DIR/sqlite-wal-fixture" "$WORK_DIR/server-data-manager-microg/services/data-microg/com.google.android.gms/tmp/binder-wal.db"
    GUEST_DATA="$WORK_DIR/app-data-manager-microg"
    GUEST_SERVER_DATA="$WORK_DIR/server-data-manager-microg"
    GUEST_PICTURES="$RESULT_DIR/manager-microg-client-pictures"
    GUEST_SERVER_PICTURES="$RESULT_DIR/manager-microg-server-pictures"
    export GUEST_DATA GUEST_SERVER_DATA GUEST_PICTURES GUEST_SERVER_PICTURES
    if ! dbus-run-session -- "$WORK_DIR/fixture" --manager-guests > "$RESULT_DIR/manager-microg-guests.log" 2>&1; then
        tail -n 120 "$RESULT_DIR/manager-microg-guests.log"
        exit 1
    fi
    if ! grep -q 'ManagerBoundBinder: UID 16464 PID 50000' "$RESULT_DIR/manager-microg-guests.log" ||
       ! grep -q 'BinderGuestTest: PASS manager-bound Binder UID and PID' "$RESULT_DIR/manager-microg-guests.log" ||
       ! grep -q 'BinderGuestTest: PASS cross-process real microG broker and callback' "$RESULT_DIR/manager-microg-guests.log" ||
       ! grep -q 'BinderGuestTest: PASS self-hosted broker rejected another package' "$RESULT_DIR/manager-microg-guests.log" ||
       ! grep -q 'PASS headless runtime stopped without an activity' "$RESULT_DIR/manager-microg-guests.log" ||
       grep -Eq 'BinderGuestTest: FAIL|Binder transport shutdown failed|run: the app has ended \(status -|FAIL:' "$RESULT_DIR/manager-microg-guests.log"; then
        tail -n 120 "$RESULT_DIR/manager-microg-guests.log"
        exit 1
    fi
    echo "Real microG broker served a manager-bound caller identity"
fi
