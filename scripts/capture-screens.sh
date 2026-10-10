#!/bin/sh
# Captures an application's screens on Linux SDL, headless.
#
#   scripts/capture-screens.sh <app-directory> [WIDTHxHEIGHT]
#
# Runs the app built for an SDL board with SDL's offscreen video driver, its
# touch input replayed from <app-directory>/screens.touch, and writes each
# "shot NAME" step to <app-directory>/screens/NAME.ppm, plus NAME.png when
# ImageMagick's convert is installed. The size defaults to 240x320, the
# RP2350 Touch LCD 2.8's panel. Build first, configured for Linux SDL:
#
#   ./scripts/configure-linux.sh && ./scripts/build.sh
#
# The platform.linux.sdl provider supplies the controls used here:
# MM_SDL_DISPLAY_SIZE, MM_TOUCH_SCRIPT, MM_SCREENSHOT_DIR, and
# MM_SCREENSHOT_EVERY=0, so only the script's shots are written.

set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: capture-screens.sh <app-directory> [WIDTHxHEIGHT]" >&2
    exit 64
fi

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
app=${1%/}
size=${2:-240x320}
app_dir="$root/$app"
script="$app_dir/screens.touch"

if [ ! -f "$app_dir/mm.mdy" ]; then
    echo "capture-screens: $app_dir has no mm.mdy" >&2
    exit 65
fi
if [ ! -f "$script" ]; then
    echo "capture-screens: $script is missing; write the app's touch script first" >&2
    exit 65
fi

# The executable is named by the manifest's name key.
name=$(sed -n 's/^name:[[:space:]]*//p' "$app_dir/mm.mdy" | head -n 1)
executable=""
for candidate in "$root"/out-target-*-linux-gnu/"$app"/"$name"; do
    if [ -x "$candidate" ]; then
        executable=$candidate
        break
    fi
done
if [ -z "$executable" ]; then
    echo "capture-screens: no Linux build of $name under $root/out-target-*-linux-gnu/$app" >&2
    echo "configure for Linux SDL and build it first" >&2
    exit 65
fi

screens="$app_dir/screens"
mkdir -p "$screens"

# The script's exit step closes the run, which an application reports as a
# failed touch read; its exit status is therefore not a capture failure.
set +e
(cd "$app_dir" &&
    SDL_VIDEODRIVER=offscreen \
    MM_SDL_DISPLAY_SIZE="$size" \
    MM_SCREENSHOT_DIR="$screens" \
    MM_SCREENSHOT_EVERY=0 \
    MM_TOUCH_SCRIPT="$script" \
    timeout 60 "$executable")
status=$?
set -e
if [ "$status" -eq 124 ]; then
    echo "capture-screens: $name did not finish within 60 s; does its script end with exit?" >&2
    exit 1
fi

found=0
for ppm in "$screens"/*.ppm; do
    [ -f "$ppm" ] || continue
    found=$((found + 1))
    if command -v convert >/dev/null 2>&1; then
        convert "$ppm" "${ppm%.ppm}.png"
    fi
done
if [ "$found" -eq 0 ]; then
    echo "capture-screens: $name wrote no screens" >&2
    exit 1
fi
echo "capture-screens: $found screen(s) of $name in $screens"
