#!/bin/sh
set -eu

app_root=$(CDPATH= cd -- "$(dirname -- "$0")/../FileCommander" && pwd)
config="$app_root/out/config.mdy"
if [ ! -f "$config" ]; then
    echo 'FileCommander: configure and build the Linux target first' >&2
    exit 65
fi
build_dir=$(sed -n 's/^target-build-directory: *//p' "$config")
case "$build_dir" in
    out-target-*) ;;
    *) echo 'FileCommander: expected a configured target output directory' >&2; exit 65 ;;
esac
binary="$app_root/$build_dir/filecommander"
if [ ! -x "$binary" ]; then
    echo 'FileCommander: target executable missing; build the app first' >&2
    exit 65
fi
mkdir -p "$app_root/data"
cd "$app_root/data"
exec "$binary"
