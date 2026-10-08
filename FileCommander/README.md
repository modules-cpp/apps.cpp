# FileCommander

FileCommander browses the board's local file system with a touch display. Pico
boards use LittleFS in their reserved internal flash region. Linux SDL uses the
selected local directory through the same `mm.fs` API.

Touch an entry to select it, then OPEN to enter a folder or preview a file. UP
returns to the parent. PREV and NEXT page through directories of any size. NEW
creates an empty file; DIR creates a folder. RENAME edits the selected name.
DELETE requires a second confirmation tap. In file preview, APPEND adds up to
24 text characters at a time. Preview displays the first 512 bytes; non-text
bytes appear as dots. Files and directories retain their contents across runs.

The default display theme is amber phosphor on black. Tap the AMBER > label
in the list header to switch to green phosphor; tap GREEN > to switch back.
The selected row and on-screen buttons follow the chosen theme. Monochrome
displays show the same interface in black and white.

The on-screen keyboard enters uppercase letters, digits, period, hyphen,
underscore and space. The underlying file API supports longer names, but this
first UI edits up to 24 characters. Deleting a nonempty folder fails safely.
No copy/move between volumes, binary editor or recursive delete is provided.

The app first mounts without formatting. If Pico flash has an unformatted
LittleFS region, it shows FORMAT FLASH. Confirming that action formats the
reserved storage region. Do not confirm if you need to recover existing data.
A failed mount for any other reason remains visible and does not format the
device.

Set `MM_MODULES` to a built modules.cpp installation. From `FileCommander/`,
for Linux SDL:

```sh
../scripts/configure-linux.sh
../scripts/build.sh
../scripts/run-filecommander-linux.sh
```

The Linux run script launches the built executable from `FileCommander/data/`.
That directory is the host storage sandbox; it is created on first run. The
generic `../scripts/run.sh` changes the working directory to the app source,
so it should not be used for this file manager on Linux.

For the RP2350-Touch-LCD-2.8:

```sh
../scripts/configure_rp2350_touch_lcd_28.sh
export picotool_DIR=/absolute/path/to/picotool-package
../scripts/build.sh
../scripts/flash.sh
```

For the RP2350-Touch-LCD-1.54, use the same build and flash commands after
configuring its board:

```sh
"$MM_MODULES/configure" --target arm-none-eabi \
    --compiler arm-none-eabi-g++ --c-compiler arm-none-eabi-gcc \
    --sdk pico-arm --board rp2350_touch_lcd_154 --build debug .
../scripts/build.sh
../scripts/flash.sh
```

The board's touch and display providers must be selected. The supported panel
range is 220–480 pixels wide and 240–320 pixels high, with 1-bit or RGB565
output. Other Pico boards can be used when their display, touch, local storage
and delay providers are configured and the panel fits that range.
