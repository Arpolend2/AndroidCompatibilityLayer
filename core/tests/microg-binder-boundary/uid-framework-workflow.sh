#!/bin/bash
set -eu
export MEGLER_JDK="$(cygpath -u "$MEGLER_JDK")"
export MEGLER_ANDROID_SDK="$(cygpath -u "$MEGLER_ANDROID_SDK")"
exec "$(cygpath -u "$1")" --force
