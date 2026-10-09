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
