#!/bin/bash
$MM_MODULES/v1.3.x/configure \
        --target aarch64-linux-gnu --target-host \
        --compiler aarch64-linux-gnu-g++ --c-compiler aarch64-linux-gnu-gcc \
        --sdk linux-aarch64 --board sdl-linux-aarch64 \
        --runner native --build debug .

