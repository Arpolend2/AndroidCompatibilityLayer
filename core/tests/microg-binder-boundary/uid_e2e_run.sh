#!/bin/sh
set -eu
: "${MANAGER_FIXTURE:?Set MANAGER_FIXTURE to manager_lifecycle_test}"
: "${MANAGER_EXECUTABLE:?Set MANAGER_EXECUTABLE to the self-spawning manager}"
: "${CLIENT_EXECUTABLE:?Set CLIENT_EXECUTABLE to shared-microg-startup-client}"
: "${MICROG_APK:?Set MICROG_APK to the original microG APK}"
: "${ANDROID_ROOT:?Set ANDROID_ROOT to the fresh framework root}"
: "${CLIENT_APKS:?Set CLIENT_APKS to the directory containing client1.apk and client2.apk}"
: "${RESULT_DIR:?Set RESULT_DIR to a new empty directory}"
if [ ! -f /.dockerenv ]; then
    echo "This fixture must run in a disposable Docker container" >&2
    exit 1
fi
test -d "$RESULT_DIR"
test -z "$(ls -A "$RESULT_DIR")"
id1=ru.androidcompatibilitylayer.Android0123456789abcdef00000001
id2=ru.androidcompatibilitylayer.Android0123456789abcdef00000002
manager=/usr/bin/ru.androidcompatibilitylayer.appmanager
test ! -e "$manager"
test ! -e "/usr/bin/$id1"
test ! -e "/usr/bin/$id2"
cp "$MANAGER_FIXTURE" "$manager"
cp "$CLIENT_EXECUTABLE" "/usr/bin/$id1"
cp "$CLIENT_EXECUTABLE" "/usr/bin/$id2"
mkdir "$RESULT_DIR/worker-data" "$RESULT_DIR/apps" "$RESULT_DIR/client1-data" \
      "$RESULT_DIR/client2-data" "$RESULT_DIR/pictures"
printf 'megler.test.sharedclient1\t\tClient 1\t1\t1\nmegler.test.sharedclient2\t\tClient 2\t1\t1\n' > "$RESULT_DIR/apps/apps.txt"
: > "$RESULT_DIR/manager.log"
: > "$RESULT_DIR/client1.log"
: > "$RESULT_DIR/client2.log"
mkfifo "$RESULT_DIR/control"
exec 3<>"$RESULT_DIR/control"
manager_pid=
client1_pid=
client2_pid=
cleanup() {
    for pid in "$client1_pid" "$client2_pid" "$manager_pid"; do
        if [ -n "$pid" ]; then kill "$pid" 2>/dev/null || true; fi
    done
}
trap cleanup EXIT
ACL_SHARED_MICROG_MANAGER_EXECUTABLE="$MANAGER_EXECUTABLE" "$manager" \
    --shared-microg-manager-fixture "$MICROG_APK" "$ANDROID_ROOT" "$RESULT_DIR/worker-data" \
    "$RESULT_DIR/apps" "$id1=megler.test.sharedclient1" "$id2=megler.test.sharedclient2" \
    <"$RESULT_DIR/control" 3>&- >"$RESULT_DIR/manager.log" 2>&1 &
manager_pid=$!
tries=0
while ! grep -q SHARED_MICROG_MANAGER_FIXTURE_READY "$RESULT_DIR/manager.log"; do
    if ! kill -0 "$manager_pid" 2>/dev/null || [ "$tries" -ge 60 ]; then
        cat "$RESULT_DIR/manager.log"
        exit 1
    fi
    tries=$((tries + 1))
    sleep 1
done
"/usr/bin/$id1" "$CLIENT_APKS/client1.apk" "$RESULT_DIR/client1-data" "$ANDROID_ROOT" \
    "$RESULT_DIR/pictures" 60000 3>&- >"$RESULT_DIR/client1.log" 2>&1 &
client1_pid=$!
"/usr/bin/$id2" "$CLIENT_APKS/client2.apk" "$RESULT_DIR/client2-data" "$ANDROID_ROOT" \
    "$RESULT_DIR/pictures" 60000 3>&- >"$RESULT_DIR/client2.log" 2>&1 &
client2_pid=$!
ok=1
tries=0
while ! grep -q 'SharedMicrogUidTest: PASS megler.test.sharedclient1' "$RESULT_DIR/client1.log" ||
      ! grep -q 'SharedMicrogUidTest: PASS megler.test.sharedclient2' "$RESULT_DIR/client2.log"; do
    if ! kill -0 "$client1_pid" 2>/dev/null || ! kill -0 "$client2_pid" 2>/dev/null ||
       [ "$tries" -ge 60 ]; then ok=0; break; fi
    tries=$((tries + 1))
    sleep 1
done
if ! kill -0 "$client1_pid" 2>/dev/null || ! kill -0 "$client2_pid" 2>/dev/null; then ok=0; fi
wait "$client1_pid" || ok=0
client1_pid=
wait "$client2_pid" || ok=0
client2_pid=
exec 3>&-
wait "$manager_pid" || ok=0
manager_pid=
for n in 1 2; do
    log="$RESULT_DIR/client$n.log"
    package="megler.test.sharedclient$n"
    manager_uid=$(sed -n "s/.*manager UID=\\([0-9][0-9]*\\) PID=[0-9][0-9]* package=$package.*/\\1/p" "$log" | sort -u)
    guest_uid=$(sed -n "s/.*SharedMicrogUidTest: PASS $package \\([0-9][0-9]*\\).*/\\1/p" "$log" | sort -u)
    if [ -z "$manager_uid" ] || [ "$manager_uid" != "$guest_uid" ] ||
       grep -q 'SharedMicrogUidTest: FAIL' "$log"; then ok=0; fi
    if [ "$n" = 1 ]; then uid1=$manager_uid; else uid2=$manager_uid; fi
done
if [ "$uid1" = "$uid2" ]; then ok=0; fi
if ! grep -Fq "UID [$uid1] is not related to packageName [megler.test.sharedclient2]" "$RESULT_DIR/manager.log" ||
   ! grep -Fq "UID [$uid2] is not related to packageName [megler.test.sharedclient1]" "$RESULT_DIR/manager.log"; then ok=0; fi
if [ "$ok" != 1 ]; then
    tail -n 100 "$RESULT_DIR/manager.log" "$RESULT_DIR/client1.log" "$RESULT_DIR/client2.log"
    exit 1
fi
printf 'End-to-end microG UID checks passed: %s=%s %s=%s\n' \
    megler.test.sharedclient1 "$uid1" megler.test.sharedclient2 "$uid2"
