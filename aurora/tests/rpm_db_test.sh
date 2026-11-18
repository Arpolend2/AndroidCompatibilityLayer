#!/bin/sh
set -eu

if [ "$#" -ne 2 ] || [ ! -f "$1" ] || [ ! -d "$2" ] ||
   [ -n "$(find "$2" -mindepth 1 -maxdepth 1 -print -quit)" ]; then
    echo "Usage: rpm_db_test.sh package.rpm empty-test-root" >&2
    exit 2
fi

package=$1
root=$2
name=$(rpm -qp --qf '%{NAME}' "$package")
rpm --root "$root" --initdb
rpm --root "$root" -i --nodeps --nosignature --noscripts --notriggers "$package" >"$root/install.log" 2>&1
rpm --root "$root" -qa --qf '%{NAME}\n' >"$root/query.log" 2>&1
if [ "$(cat "$root/query.log")" != "$name" ] ||
   grep -qiE 'digest: BAD|skipping h#|error:' "$root/install.log"; then
    cat "$root/install.log" "$root/query.log" >&2
    exit 1
fi
rpm --root "$root" -V --nodeps "$name" >"$root/verify.log" 2>&1
if [ -s "$root/verify.log" ]; then
    cat "$root/verify.log" >&2
    exit 1
fi
rpm --root "$root" -e --nodeps --noscripts --notriggers "$name" >"$root/remove.log" 2>&1
rpm --root "$root" -qa >"$root/empty.log" 2>&1
if [ -s "$root/remove.log" ] || [ -s "$root/empty.log" ]; then
    cat "$root/remove.log" "$root/empty.log" >&2
    exit 1
fi
