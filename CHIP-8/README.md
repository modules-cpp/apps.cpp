# CHIP-8

This app uses modules.cpp's selected display, graphics, touch, font, and MCU
providers. There is built-in Catch the Dot ROM in this app. 
The interpreter design follows the CHIP-8 description in
[Austin Morlan's emulator guide](https://austinmorlan.com/posts/chip8_emulator/);
the code and game were written for apps.cpp.

Touch the on-screen hexadecimal keypad. In Catch the Dot, hold **4** to move
left and **6** to move right. Catch dots with the three-pixel paddle; the
single hexadecimal score digit increases on a catch and resets on a miss.

From this directory, with `MM_MODULES` pointing at a built modules.cpp
v1.3.x installation:

```sh
../scripts/configure-linux.sh
../scripts/build.sh
../scripts/run.sh
```

The Linux script selects an SDL board. For the RP2350-Touch-LCD-2.8:

```sh
../scripts/configure_rp2350_touch_lcd_28.sh
export picotool_DIR=/absolute/path/to/picotool-package
../scripts/build.sh
../scripts/flash.sh
```

The Pico SDK and Arm toolchain must already be installed. `picotool_DIR`
must contain `picotoolConfig.cmake`; a successful build creates the UF2 image
that flash consumes.

The interpreter and game have a standalone host test. From `CHIP-8/`, run:

```sh
../scripts/test.sh
```
