# FunCommander

A portable retro game emulator and launcher for CHIP-8 games stored on internal LittleFS flash storage (`/games`) and an external SD card (`/sd`) formatted with the FAT filesystem.

Designed for touch-screen embedded devices (such as the Waveshare RP2350-Touch-LCD-2.8) and hosted Linux SDL development.

## Features

- **CHIP-8 Virtual Machine**: Reuses the core engine `CHIP-8/chip8.cpp` directly: full opcode implementation, 4 KB memory, 16 registers (V0–VF), 16-level call stack, 64x32 monochrome display, 60 Hz delay and sound timers, and 700 Hz CPU stepping.
- **Internal Flash Storage (LittleFS)**: Automatically mounts the board's internal flash storage at `/games` via `mm.fs.local` (reusing LittleFS patterns from `FileCommander`). On initial boot, bundled games (`snake.ch8`, `caveexplorer.ch8`, `tank.ch8`, `1-chip8-logo.ch8`, `2-ibm-logo.ch8`, and `catch_the_dot.ch8`) are seeded onto the LittleFS filesystem so they are immediately available to select and load.
- **External SD Card (FAT Filesystem)**: Mounts the physical TF card socket at `/sd` via `mm.sdcard.socket` and `mm.fs.fat`.
- **Game Launcher & Browser**:
  - Automatically loads the `/games` LittleFS directory upon boot.
  - Lists and navigates CHIP-8 games (`.ch8`, `.c8`, `.rom`) with sizes.
  - Supports directory navigation (`..` parent, subfolders).
  - Paging support (`< PREV`, `NEXT >`) for directories with multiple games.
  - Source toggle button (`[TO SD]` / `[TO LFS]`) to seamlessly switch between internal LittleFS flash and external SD card.
- **Audio Buzzer (`mm.audio`)**: Built-in sound synthesis driven by the CHIP-8 60 Hz sound timer (`chip8::sound_timer()`). Plays authentic retro beeps over the board's I2S audio sink (`mm.audio::selected_out()`, driving the PCM5101A DAC and APA2068 amplifier on RP2350-Touch-LCD-2.8), with graceful fallback on boards without audio.
- **Touch-First Controls**:
  - In-game on-screen 4x4 hexadecimal keypad (0–F) matching standard CHIP-8 layout.
  - Real-time touch feedback (highlighting pressed keys) and continuous key-hold support for fluid controls.
  - Top menu bar with current game title, sound buzzer indicator, reset button (`RST`), and exit button (`EXIT`) to return to the launcher.

## Directory Structure

```text
FunCommander/
  mm.mdy            - Application manifest (kind: app, uses mm.audio, mm.fs.local, mm.fs.fat, mm.sdcard.socket)
  main.cpp          - UI rendering, touch handling, and game loop
  buzzer.hpp        - Audio buzzer synthesizer using mm.audio and mm::audio::Ring
  chip8.hpp         - Shared header matching CHIP-8/chip8.cpp
  launcher.hpp      - Dual LittleFS/FAT storage, directory browsing, and ROM loading interface
  launcher.cpp      - LittleFS /games and FAT /sd integration
  bundled_games.hpp - Bundled ROM binary assets seeded into /games
  games/            - CHIP-8 game ROMs (including catch_the_dot.ch8)
```

## How to Build and Run

### On Waveshare RP2350-Touch-LCD-2.8

From this directory (or the `apps.cpp` root):

```sh
../scripts/configure_rp2350_touch_lcd_28.sh
export picotool_DIR=/path/to/picotool-package
../scripts/build.sh
../scripts/flash.sh
```

The RP2350 Touch LCD 2.8 board binding provides:
- Display: ST7789 240x320 IPS panel (`platform.rp2350_touch_lcd_28.display`)
- Touch: CST328 capacitive touch controller (`platform.rp2350_touch_lcd_28.touch`)
- Internal Flash Storage: LittleFS on onboard flash (`platform.pico.fs.littlefs`)
- External Storage: 4-bit SD mode over PIO (`platform.rp2350_touch_lcd_28.sdcard`)
- SD Filesystem: FatFs R0.16 (`platform.pico.fs.fat`)

### Using the SD Card

1. Format a micro-SD / TF card with FAT16 or FAT32.
2. Copy any standard `.ch8` CHIP-8 game ROMs onto the card.
3. Insert the card into the device's slot.
4. In FunCommander, tap `[TO SD]` to switch to the SD card directory. Tap `[TO LFS]` anytime to return to the internal `/games` LittleFS flash folder.
