#!/bin/sh
# Clean the current application; --distclean also removes its configuration.
set -eu

if [ -z "${MM_MODULES:-}" ] || [ ! -x "$MM_MODULES/clean" ]; then
    echo 'clean: set MM_MODULES to a built modules.cpp installation' >&2
    exit 65
fi

exec "$MM_MODULES/clean" "$@" .
