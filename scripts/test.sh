#!/bin/sh
set -eu

if [ ! -f tests/run.sh ]; then
    printf 'test: no tests/run.sh in %s\n' "$PWD" >&2
    exit 65
fi
exec sh tests/run.sh "$@"
