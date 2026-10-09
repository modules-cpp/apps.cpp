# AudioCommander

AudioCommander is a small touch-screen synthesizer. Its first instrument is a
one-note-at-a-time piano: tap or hold one of the seven white keys or five black
keys, and use `- OCT` / `OCT +` to select octaves 3 through 6. A held note
decays; lifting your finger releases it smoothly.

The active key is drawn before its tone begins, and the idle keyboard is drawn
after the release ends. This keeps full-screen LCD transfers out of the
time-critical audio loop. Brief empty touch reports are ignored so a held key
does not repeatedly restart its note. The output drains the release samples
before stopping the speaker.

The app uses `mm.audio` with a caller-owned sample ring and the selected
`mm.display` and `mm.touch` providers. It supports the Waveshare
RP2350-Touch-LCD-1.54 (240 x 240, ES8311 codec) and RP2350-Touch-LCD-2.8
(PCM5101A DAC). The keyboard sizes itself to the display. A board without an
audio output reports `Unsupported`, and this app exits rather than showing a
silent keyboard. Linux SDL currently has no `mm.audio` output provider.

From this directory, with `MM_MODULES` pointing to a modules.cpp installation
and the external `apps.cpp` tree configured for the board:

```sh
$MM_MODULES/build --target mm.mdy
$MM_MODULES/flash mm.mdy
```

The generated application is `audiocommander`. Audio and touch behavior still
need verification on physical hardware.
