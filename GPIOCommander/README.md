# GPIOCommander

GPIOCommander manages the chip's GPIOs from the touch screen. It has four
tabs:

- **PINS** lists every GPIO the board reports, three columns to a page, each
  with its mode and live value: `1`/`0` for a digital pin, volts for an ADC
  pin, duty for a PWM pin. Mode letters: `i` input, `o` output, `a` ADC,
  `p` PWM, `-` not yet configured, `r` reserved. Tap a pin to open it.
- **PIN** configures one pin: input (`PULL` cycles none, up, down), output
  (`SET HIGH`/`SET LOW`), ADC, or 1 kHz PWM with `DUTY -`/`DUTY +` in 10 %
  steps. `SCOPE` opens the oscilloscope on the pin's ADC channel, `LOGIC` puts
  the pin on the logic analyzer's first lane.
- **SCOPE** is a single-channel oscilloscope on the ADC. `CH` cycles the
  permitted channels, `RATE-`/`RATE+` choose 500 S/s to 500 kS/s (capped at
  the channel's paced maximum), `TRIG` toggles a rising midpoint trigger, and
  `RUN`/`HOLD` freezes the trace. It shows min, max, and mean in volts, and a
  frequency estimate. Where the platform can pace the converter the capture
  is hardware-timed; otherwise it is polled (marked `POLL`, at most 20 kS/s).
- **LOGIC** is a four-lane logic analyzer. Tap a lane's name to move it to
  the next GPIO. `RATE` cycles the sample rate from 1 kS/s to 500 kS/s, or
  `MAX` for as fast as the loop runs; `TRIG` waits up to two seconds for a
  rising or falling edge on lane 1; `RUN` captures 2048 samples; `<` `>` pan
  and `ZOOM` shows 1 to 16 samples per pixel. The header shows the rate
  actually achieved and `LATE` counts samples taken after their slot.

## Reserved pins

On the Pico, configuring a GPIO re-initialises its pad whatever peripheral
owns it, so one wrong tap could disconnect the display or the SD card. The
app keeps a table of the pins each supported board wires to itself --
display, touch, codec, I2C, SD socket, keys, battery -- taken from the
board documentation in modules.cpp, and never configures them. They are
still read, so their levels show on PINS and the logic analyzer can watch
them, for example the LCD's SPI clock. A battery-sense pin may also be read
by the ADC.

| Board | Free pins |
|---|---|
| RP2350-Touch-LCD-2.8 | GP0, GP1 (exposed UART), GP5, GP8, GP9, GP12, GP28, GP29 |
| RP2350-LCD-1.54 and Touch-LCD-1.54 | GP8, GP26, GP27 (exposed UART) |

On any other board every pin is reserved and the app is a monitor only. A
free pin may still be unconnected on the board; only the exposed UART pins
are on a connector on these boards.

## Limits

- Logic analyzer timing comes from a software loop reading each lane through
  `mm.mcu`, not from PIO, so its true ceiling depends on the board and is
  shown as the achieved rate. Short pulses between samples are missed.
- The oscilloscope has one channel and no pre-trigger history; the trigger
  is searched for in the first half of each capture.
- PWM runs at 1 kHz. Two pins on one PWM slice cannot run at different
  periods; the second answers `PWM SLICE BUSY`.
- A touched pin keeps its last configuration until the app exits.

## Build

From this directory, with `MM_MODULES` pointing to a modules.cpp installation
and the external `apps.cpp` tree configured for a supported board:

```sh
$MM_MODULES/build --target mm.mdy
$MM_MODULES/flash mm.mdy
```

The generated application is `gpiocommander`. Its behaviour still needs
verification on physical hardware.
