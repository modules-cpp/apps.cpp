# MIDICommander

MIDICommander browses and plays Standard MIDI Files from internal LittleFS and
an SD card on the Waveshare RP2350-Touch-LCD-2.8. It reads LittleFS at `/data`
(auto-formatting if blank and seeding demo songs) and FAT SD at `/sd`. Use
**TO SD / TO LFS** to change source, select a file, then tap **OPEN / PLAY** or
double-tap the row. Tap **INFO** beside OPEN / PLAY to inspect file metadata
(SMF format, track count, PPQ division, tempo in BPM, duration, note count,
and title/sequence name). Directories can be opened the same way; **UP**, **PREV**,
and **NEXT** navigate. While playing, **VOL-** and **VOL+** adjust the synthesis
volume (with visual gauge), **STOP** ends playback, and **INFO** opens the
metadata screen.

If **TO SD** fails, the status line now reports the filesystem result. “SD
needs FAT16/32” means the card was reached but no supported FAT volume was
found (for example, an exFAT-formatted card); “SD card I/O error” means the
card or SDIO transfer could not be read. “SD card init timeout” means card
identification did not finish; “SD FAT mount timeout” means the first card
identification worked but FAT mounting subsequently timed out. Source
switching leaves the current browser path intact on failure. Insert the card
before tapping **TO SD** again
to retry a failed mount.

The player supports Standard MIDI File format 0 and 1, up to 32 tracks, a
positive ticks-per-quarter-note division, running status, tempo changes,
polyphonic note on/off, and sustain pedal messages. It accepts `.mid` and
`.midi` names, case-insensitively. Up to eight simultaneous pitched voices use
AudioCommander's piano waveform through `mm.audio`. Percussion channel
10 and instrument program changes are not synthesized. Format 2, SMPTE timing,
system-common events, and files larger than 128 KiB are reported as
unsupported. MIDI playback is audio synthesis, not a MIDI hardware output.

Playback keeps LCD redraws outside the active audio loop so screen transfers
do not starve the RP2350 I2S output.

With `MM_MODULES` pointing to a built modules.cpp installation, configure the
`apps.cpp` tree for the RP2350 Touch LCD 2.8 and build from this directory:

```sh
../scripts/configure_rp2350_touch_lcd_28.sh
export picotool_DIR=/path/to/picotool-package
$MM_MODULES/build --target MIDICommander
$MM_MODULES/flash MIDICommander
```

Built-in demo songs (`duck_strut.mid` and `outer_wilds.mid`) are automatically
seeded to LittleFS `/data` on first boot, so playback works immediately even
without an SD card. Additional MIDI files can be added to `/data` or copied to a
FAT-formatted SD card inserted in the board socket.
