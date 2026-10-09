# apps.cpp

C++ applications built outside the modules.cpp checkout.

## Set up modules.cpp

Build the modules.cpp host tools first in the installation you want to use.
Then set `MM_MODULES` to the **absolute path of that installation**, not to
this apps.cpp repository:

```sh
export MM_MODULES=/absolute/path/to/modules.cpp
test -x "$MM_MODULES/out/bin/configure"
```

The scripts in `scripts/` invoke `$MM_MODULES/configure`,
`$MM_MODULES/build`, `$MM_MODULES/run`, or `$MM_MODULES/flash`.
Set `MM_MODULES` in each new terminal, or add the export to your shell startup
file. A local `.env` file is not loaded automatically.

## Build all applications

The root `mm.mdy` connects AudioCommander, CHIP-8, FileCommander,
FunCommander, GPIOCommander, MIDICommander, clockal, fractals, and kalkulator. From this
repository's root, configure and build them
for Linux SDL:

```sh
./scripts/configure-linux.sh
./scripts/build.sh
```

The shared configuration is `out/config.mdy`; executables are under
`out-target-*/<app-directory>/`. Run an individual app from its directory.
For FileCommander, use `./scripts/run-filecommander-linux.sh` from the root,
which keeps its Linux files in `FileCommander/data/`.
Clockal reads `mm.rtc` for its seven-segment clock and date. See
`clockal/README.md` for the supported Pico board and RTC trust behavior.
AudioCommander needs a board with display, touch, and an `mm.audio` speaker;
the RP2350 Touch LCD 2.8 is the initial target. See
`AudioCommander/README.md` for its touch piano controls.
MIDICommander adds read-only LittleFS and FAT SD browsing for MIDI file
playback on the same board; see `MIDICommander/README.md` for format limits.
GPIOCommander shows and configures the chip's GPIOs, with an ADC
oscilloscope and a four-lane logic analyzer; see `GPIOCommander/README.md`
for the pins it reserves on each board.

## Build and run fractals on Linux

From this repository's `fractals/` directory:

```sh
cd fractals
../scripts/configure-linux.sh
../scripts/build.sh
../scripts/run.sh
```

The Linux configure script selects the native SDL board from the C++ compiler's
reported target. It uses `clang++` when available, otherwise `g++`. Set
`CXX` to choose another installed compiler, for example
`CXX=clang++-21 ../scripts/configure-linux.sh`. The SDL2 development package
must be installed to build the desktop display provider.

Configure writes the shared `apps.cpp/out/config.mdy`. Build and run use that
configuration; they do not configure the modules.cpp installation.

## Build and flash the RP2350 touch LCD board

From `fractals/`, with the Pico SDK and Arm toolchain available:

```sh
../scripts/configure_rp2350_touch_lcd_28.sh
export picotool_DIR=/absolute/path/to/picotool-package
../scripts/build.sh
../scripts/flash.sh
```

`picotool_DIR` names the directory containing `picotoolConfig.cmake` and the
`picotool` executable. The external build requires this variable regardless of where
picotool is installed. A failed build does not produce a flashable UF2; fix
the build error before running flash.
# ServoCommander

ServoCommander controls the Waveshare Pico Servo Driver through its 16 direct
Pico GPIO PWM outputs. It reads commands from the selected `mm.stdio` console;
with the Pico SDK board configuration this is USB CDC stdio.

The output mapping is channel 0–15 to GP0–GP15, following the Waveshare board
schematic. Each output runs at 50 Hz. Startup commands 1500 us (the nominal
center pulse) on every available channel.

Connect a serial terminal to the Pico USB CDC port and use these commands:

```text
help
list
set 0 1500
get 0
all 1500
off 0
off all
quit
```

Pulse widths are accepted from 1000 through 2000 microseconds. `get` and `list`
show the commanded output value held by the application. Standard RC servos
have no position feedback, so this cannot verify the servo's physical angle.
`off` releases PWM and stops sending pulses; `quit` leaves current output states
unchanged.

Servo power is supplied through the driver's servo power input. Use a suitable
external supply for the servos and connect its ground to the Pico/driver ground;
do not expect the Pico USB connection to power a bank of servos.

Build and flash using the apps.cpp scripts after configuring the Pico target.
