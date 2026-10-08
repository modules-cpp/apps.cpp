#!/bin/sh
set -eu

if [ -z "${MM_MODULES:-}" ]; then
    echo 'test: set MM_MODULES to a built modules.cpp installation' >&2
    exit 65
fi
modules=$(cd "$MM_MODULES" && pwd)
if [ ! -x "$modules/out/bin/test" ] || [ ! -x "$modules/out/bin/configure" ] || [ ! -f "$modules/out/config.mdy" ]; then
    echo 'test: MM_MODULES needs out/bin/test, out/bin/configure and out/config.mdy' >&2
    exit 65
fi
host_selected=false
for arg in "$@"; do
    case "$arg" in
        --host) host_selected=true ;;
        -v|--verbose|--compile-only) ;;
        *) echo 'usage: test.sh [--host] [-v|--verbose] [--compile-only]' >&2; exit 64 ;;
    esac
done

# External application roots do not yet accept kind:test manifests. Stage
# a small project so the ordinary test tool builds and runs this suite.
app=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
workspace=$(mktemp -d "${TMPDIR:-/tmp}/chip8-tests.XXXXXX")
trap 'rm -rf -- "$workspace"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$workspace/out" "$workspace/tests"
cp "$app/chip8.cpp" "$app/game.cpp" "$workspace/"
cp "$app/tests/mm.mdy" "$app/tests/chip8.cpp" "$app/tests/main.cpp" "$workspace/tests/"
cp -R "$modules/modules/mm/test" "$workspace/framework"
cat > "$workspace/mm.mdy" <<'MANIFEST'
---
mm: 1.3
kind: project
name: chip8-tests
folder: framework
folder: tests
---
MANIFEST
cd "$workspace"
# Reuse only the configured host compiler pair. Target SDK and board paths
# belong to the installation and must not enter this temporary project.
host_compiler=$(sed -n 's/^host-compiler: *//p' "$modules/out/config.mdy")
host_c_compiler=$(sed -n 's/^host-c-compiler: *//p' "$modules/out/config.mdy")
if [ -z "$host_compiler" ] || [ -z "$host_c_compiler" ]; then
    echo 'test: installation configuration is missing its host compiler pair' >&2
    exit 65
fi
"$modules/out/bin/configure" --host --compiler "$host_compiler" \
    --c-compiler "$host_c_compiler" --build debug .
# --host is fixed: these are VM unit tests, including for a Pico-configured app.
# Accept an explicit --host without passing the option twice.
if [ "$host_selected" = false ]; then set -- --host "$@"; fi
"$modules/out/bin/test" "$@" tests/mm.mdy
