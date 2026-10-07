#!/bin/sh
set -eu

compiler=${CXX:-c++}
test_exe=$(mktemp "${TMPDIR:-/tmp}/chip8-test.XXXXXX")
trap 'rm -f -- "$test_exe"' EXIT

"$compiler" -std=c++20 -Wall -Wextra -Werror \
    chip8.cpp game.cpp tests/chip8.cpp -o "$test_exe"
"$test_exe"
printf 'CHIP-8 tests passed\n'
