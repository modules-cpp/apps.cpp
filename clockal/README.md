# Clockal

Clockal shows 24-hour time in seven-segment digits and the current date beneath
it. It reads the clock through `mm.rtc`. Tap the theme name at the top to switch
between amber and green phosphor on a dark background. The layout adapts to
the 240 x 240 and 240 x 320 Pico panels and the 480 x 320 Linux SDL window.

The RP2350-Touch-LCD-2.8 uses its PCF85063 battery-backed RTC. If its oscillator
has stopped, Clockal shows dashes and **CLOCK NEEDS SETTING**. An invalid
calendar also displays dashes with **INVALID RTC DATE**. The
RP2350-Touch-LCD-1.54 has no RTC. Its `mm.rtc` provider uses the monotonic
millisecond counter to run a software clock after `tdset` supplies the UTC date
and time. Until then Clockal shows dashes and **CLOCK NEEDS SETTING**. The
software clock cannot track time while the board is off and must be set again
after every reset or power cycle.

Linux SDL uses the `mm.rtc` system-clock provider. Its default convention is
UTC, so the screen labels it **UTC SYSTEM TIME**. It is usable even when the
provider's hardware-trust flag is false; that flag describes RTC validation,
not whether the Linux system clock can be read.

Set `MM_MODULES` to a built modules.cpp installation. From `clockal/`, build
and run on Linux:

```sh
../scripts/configure-linux.sh
../scripts/build.sh
../scripts/run.sh
```

For the RP2350-Touch-LCD-2.8:

```sh
../scripts/configure_rp2350_touch_lcd_28.sh
export picotool_DIR=/absolute/path/to/picotool-package
../scripts/build.sh
../scripts/flash.sh
```

For the RP2350-Touch-LCD-1.54, use
`../scripts/configure_rp2350_touch_lcd_154.sh` in place of the 2.8 configure
script before building and flashing.

To set the board's clock from your Linux computer, connect the board by USB,
run Clockal on it, and find its CDC serial device (usually `/dev/ttyACM0`).
Then run:

```sh
./tdset /dev/ttyACM0
```

`tdset` uses the computer's current **UTC** time. Clockal displays that UTC
value directly on the board; it does not apply a timezone offset. The utility
first checks that the device is running Clockal, then sends the time and waits
for confirmation from the hardware or software clock. It does not change the
computer's clock.
If you receive a permission error, your Linux user needs access to the serial
device. If several USB serial devices exist, pass the one belonging to the
board; `ls -l /dev/serial/by-id/` can help identify it.

This app is included in the parent `apps.cpp/mm.mdy` tree, so configuring from
the app directory or repository root writes the same `apps.cpp/out/config.mdy`.
