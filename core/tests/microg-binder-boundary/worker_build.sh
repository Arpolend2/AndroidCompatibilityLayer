#!/bin/sh
set -eu
: "${WORK_DIR:?Set WORK_DIR to an isolated build directory}"
mkdir -p "$WORK_DIR/objects"
export CFLAGS="-std=gnu11 -O1 -D_GNU_SOURCE -ffunction-sections -fdata-sections -DMG_HAVE_SQLITE=1 -Icore/third_party/zlib -Ibuild/sqlite"
export WORK_DIR
find core/src -maxdepth 3 -name '*.c' |
    xargs -P 8 -I@ sh -c 'gcc $CFLAGS -c "$1" -o "$WORK_DIR/objects/$(echo "$1" | tr / _).o"' _ @
for source in core/third_party/zlib/*.c; do
    case "$source" in */gz*) continue;; esac
    gcc $CFLAGS -c "$source" -o "$WORK_DIR/objects/$(basename "$source").o"
done
gcc $CFLAGS -DMEGLER_SHARED_MICROG_WORKER_STANDALONE -c aurora/shared_microg_worker.c -o "$WORK_DIR/objects/worker.o"
gcc -Wl,--gc-sections -o "$WORK_DIR/megler-shared-microg-worker" "$WORK_DIR"/objects/*.o -lm -lpthread -ldl
