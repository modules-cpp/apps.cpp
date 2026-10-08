#!/bin/sh
# Flash the caller's application; pass flash options such as --auto-flash.
set -eu

if [ -z "${MM_MODULES:-}" ] || [ ! -x "$MM_MODULES/flash" ]; then
    echo 'flash: set MM_MODULES to a built modules.cpp installation' >&2
    exit 65
fi

exec "$MM_MODULES/flash" "$@"
