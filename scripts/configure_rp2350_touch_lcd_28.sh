#!/bin/bash
$MM_MODULES/configure \
	--target arm-none-eabi \
	--compiler arm-none-eabi-g++ --c-compiler arm-none-eabi-gcc \
	--sdk pico-arm --board rp2350_touch_lcd_28 \
	--build debug .
