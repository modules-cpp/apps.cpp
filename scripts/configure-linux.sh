#!/bin/sh
# Configure the caller's external app for this Linux machine's SDL board.
set -eu

if [ "$(uname -s)" != Linux ]; then
    echo "configure-linux: this script requires Linux" >&2
    exit 64
fi

if [ "$#" -gt 1 ]; then
    echo "usage: configure-linux.sh [app-manifest-or-directory]" >&2
    exit 64
fi
manifest=${1:-.}

if [ -z "${MM_MODULES:-}" ] || [ ! -x "$MM_MODULES/configure" ]; then
    echo "configure-linux: set MM_MODULES to a built modules.cpp installation" >&2
    exit 65
fi

if [ -n "${CXX:-}" ]; then
    compiler=$CXX
elif command -v clang++ >/dev/null 2>&1; then
    compiler=clang++
else
    compiler=g++
fi
if ! command -v "$compiler" >/dev/null 2>&1; then
    echo "configure-linux: C++ compiler not found: $compiler" >&2
    exit 65
fi

# The compiler determines the native target; configure validates SDK/board support.
compiler_target=$("$compiler" -dumpmachine 2>/dev/null) || {
    echo "configure-linux: cannot query target of $compiler" >&2
    exit 65
}
case "$compiler_target" in
    *-linux-*) arch=${compiler_target%%-*} ;;
    *)
        echo "configure-linux: $compiler is not a Linux compiler: $compiler_target" >&2
        exit 65
        ;;
esac

exec "$MM_MODULES/configure" \
    --target "${arch}-linux-gnu" --target-host \
    --compiler "$compiler" \
    --sdk "linux-$arch" --board "sdl-linux-$arch" \
    --runner native --build debug "$manifest"
