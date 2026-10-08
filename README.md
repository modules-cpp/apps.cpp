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

The root `mm.mdy` connects CHIP-8, FileCommander, fractals, and kalkulator.
From this repository's root, configure and build all four for Linux SDL:

```sh
./scripts/configure-linux.sh
./scripts/build.sh
```

The shared configuration is `out/config.mdy`; executables are under
`out-target-*/<app-directory>/`. Run an individual app from its directory.
For FileCommander, use `./scripts/run-filecommander-linux.sh` from the root,
which keeps its Linux files in `FileCommander/data/`.

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
