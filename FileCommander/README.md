# FileCommander

FileCommander browses the board's local file system with a touch display. Pico
boards use LittleFS in their reserved internal flash region. Linux SDL uses the
selected local directory through the same `mm.fs` API.

Tap LFS > in the title bar to select the SD card and mount its FAT filesystem
at `/sd`; tap SD > to return to `/data`. Each volume keeps its current folder.
The same list, tree, viewer, create, append, rename, and delete controls work
on either volume. The SD card must already be FAT formatted. If mounting fails,
the screen displays the mount error, and the title-bar selector remains usable.
FileCommander does not format SD cards. On a Linux SDL board without an SD socket
provider, selecting SD reports Storage unsupported.

Touch an entry to select it, then OPEN to enter a folder or view a file. UP
returns to the parent. PREV and NEXT page through directory entries. Tap
LIST > beside the path to switch to TREE >, showing the current folder's
descendants indented by depth; tap it again to return to the flat list. The
tree includes eight nested levels. OPEN on a tree folder makes it the new
root. NEW creates an empty file and DIR creates a folder in the path shown at
the top, even when a nested tree entry is selected. RENAME and DELETE act on
the selected entry; DELETE requires a second confirmation tap.

The file viewer starts in ASCII text mode and honors line breaks. Tap HEX to
see byte offsets, hexadecimal bytes, and an ASCII column; tap ASCII to switch
back. PREV and NEXT move through the file, beyond the first 512 bytes. The
viewer retains the most recent 64 page positions for backward navigation.
Nonprintable bytes appear as dots. APPEND adds up to 24 text characters at a
time. Files and directories retain their contents across runs.

The default display theme is amber phosphor on black. Tap the AMBER > label
in the list header to switch to green phosphor; tap GREEN > to switch back.
The selected row and on-screen buttons follow the chosen theme. Monochrome
displays show the same interface in black and white.

Tap INFO > beside the status line for the selected volume's information pages.
PREV and NEXT show capacity, available space, and flash geometry and LittleFS
settings on Pico, or sector geometry for SD; REFRESH rereads the live values. Cache, lookahead,
and block-cycle values are the current Pico provider's fixed configuration.
LittleFS free space
is an estimate based on allocated flash blocks, not a count of writable file
bytes. The page explicitly marks per-block wear counts and on-disk revision as
unavailable because the public storage API does not report them. On Linux,
capacity and available space refer to the host file system containing the
FileCommander data directory, not the size of that directory.

The on-screen keyboard enters uppercase letters, digits, period, hyphen,
underscore and space. The underlying file API supports longer names, but this
first UI edits up to 24 characters. Deleting a nonempty folder fails safely.
No copy/move between volumes, binary editor or recursive delete is provided.

The app first mounts without formatting. If the Pico LittleFS mount reports
Corrupt, it shows ERASE. Tap ERASE and then YES to erase the entire reserved
flash region, format it as LittleFS, and mount it. This removes every file in
that region. The action also works when the region contains nonblank data from
another filesystem or firmware. Other mount errors remain visible and do not
trigger an erase. Linux has no flash erase action.

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
The shared build configuration is in the parent `apps.cpp/out/config.mdy`.

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
