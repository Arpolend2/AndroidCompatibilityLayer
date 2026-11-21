#!/bin/sh
set -eu
: "${WORK_DIR:?Set WORK_DIR to an isolated build directory}"
: "${WORKER_OBJECTS:?Set WORKER_OBJECTS to worker_build.sh objects}"
mkdir -p "$WORK_DIR"
set --
for object in "$WORKER_OBJECTS"/*.o; do
    case "$object" in */worker.o) ;; *) set -- "$@" "$object";; esac
done
g++ -std=c++11 -fPIC -ffunction-sections -fdata-sections \
    tools/shared-microg-startup-client.cpp aurora/shared_microg_ipc.cpp \
    "$@" \
    -Wl,--gc-sections -o "$WORK_DIR/client" $(pkg-config --cflags --libs Qt5Core Qt5DBus) -lm -lpthread -ldl
