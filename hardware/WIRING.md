# Wiring

Everything the ESP32-C3 SuperMini connects to, and why each pin was chosen.
The authoritative copy of the GPIO numbers is
[`firmware/main/pins.h`](../firmware/main/pins.h) — this file explains them.

> The panel is built and its mapping is confirmed. The encoder is being wired
> now. The touch pads and the accelerometer are not connected yet — until they
> are, the pads are driven from the web page instead, through the same pin
> levels a real pad would produce.

## GPIO map

| Function | GPIO | Notes |
| --- | --- | --- |
| LED data | 10 | to board 1 `DIN`, through a 330 Ω series resistor |
| Touch, left | 3 | TTP223 `OUT`, active high |
| Touch, middle | 4 | |
| Touch, right | 5 | |
| I²C `SDA` | 6 | MPU-6050 |
| I²C `SCL` | 7 | |
| MPU-6050 `INT` | 1 | optional; wake on tap |
| Encoder `A` | 20 | |
| Encoder `B` | 21 | |
| Encoder switch | 0 | to ground, internal pull-up |

## Why these pins

**GPIO2, 8 and 9 are strapping pins** on the ESP32-C3 and are deliberately left
unused. Pulling one at boot changes the boot mode or the log output.

**GPIO20 and 21 are UART0.** They are free here only because the SuperMini has
no USB-serial bridge — the console and the flashing port are the C3's own USB
Serial/JTAG. `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` in `sdkconfig.defaults` is
what keeps them free. Turn that off and the encoder will fight the log output.

**GPIO0 is not a strapping pin on the C3** (unlike the original ESP32), so it is
safe for the encoder switch.

**The C3 has no touch-sensor peripheral** — `SOC_TOUCH_SENSOR_SUPPORTED` is not
defined for this target. That is why the three touch zones are external TTP223
modules reading as plain digital inputs rather than the ESP32's built-in
capacitive sensing.

**The C3 also has no PCNT peripheral**, so there is no hardware quadrature
counter for the encoder. `SOC_PCNT_SUPPORTED` is defined for the S3 and absent
here. The encoder must be decoded in a GPIO interrupt: polling at the 10 ms
frame tick drops detents, because a brisk flick of 20 detents per second puts
an edge every 12.5 ms.

## The encoder module

A bare EC11 has five pins and no power: three on one side (A, common, B) and
two on the other for the switch. Most of what you can actually buy is a KY-040
style **breakout board** instead, which adds a small PCB and labels the pins
`GND` `+` `S1` `S2` `KEY` — or `GND` `+` `SW` `DT` `CLK`, same thing.

| Module pin | Goes to | |
| --- | --- | --- |
| `GND` | GND | |
| `S1` (or `CLK`) | **GPIO20** | quadrature A |
| `S2` (or `DT`) | **GPIO21** | quadrature B |
| `KEY` (or `SW`) | **GPIO0** | the push switch |
| `+` (or `5V`) | **nothing** | see below |

### Leave the `+` pin unconnected

That pin exists only to feed two 10 kΩ pull-up resistors on the module, from
`+` to `S1` and `S2`. The firmware enables the ESP32-C3's *internal* pull-ups
on those pins instead, so the encoder works with four wires and the fifth is
redundant.

It is not merely redundant, though. **The ESP32-C3's GPIOs are 3.3 V and are
not 5 V tolerant.** Wiring that pin to 5 V would pull S1, S2 and KEY up to 5 V
through those resistors and drive 5 V straight into three GPIOs. If you want
the module's own pull-ups for any reason, connect `+` to **3.3 V** — never 5 V.

This is why the wiring has no encoder supply: the bare EC11 the enclosure is
modelled around has no power pin at all, and the module's is one you should not
use here.

### If the direction comes out backwards

Swap `S1` and `S2`. Which line is A and which is B only sets the sense of
rotation, and getting it the wrong way round is the expected outcome of a
coin flip rather than a mistake.

### The module will not fit the case

The enclosure pocket is sized for a bare EC11 lying behind the top wall, not
for a breakout PCB. Either desolder the encoder from the module and wire it
directly — the four connections above are the same — or switch the model to its
`KY040_BACK` layout, which puts the module flat under board 3 with the shaft
through the back. For bench work on jumper wires, none of this matters.

## Power

```
USB-C breakout  5V ──┬── 1000 µF ──┬── panel 5V (all three boards)
                     │             └── ESP32-C3 5V pin
                     └── 1N5819 ──> (blocks back-feed into the ESP32's
                                     own USB port when both are plugged in)
GND ─────────────────┴── common to everything
```

- **330 Ω** in series with the LED data line, close to the ESP32, to damp
  reflections on the first edge.
- **1000 µF** across the supply at the panel end, to absorb the inrush when a
  frame turns a lot of LEDs on at once.
- **1N5819** so that plugging the ESP32's own USB port in while the barrel
  supply is connected does not back-feed 5 V into the host.
- Feed 5 V to **each of the three boards** rather than daisy-chaining power
  through the board-to-board links. At 2.5 A the copper in those links is the
  weak point, and the far board browns out first.

## Panel chain

Three 65 mm 8×8 WS2812B boards, chained left to right as you face the panel:

```
ESP32 GPIO10 ──330Ω──> [board 1] DOUT ──> [board 2] DOUT ──> [board 3]
```

How the LEDs are wired *inside* one board varies between suppliers, which is
why `kWiring` in `firmware/components/panel/include/panel/config.h` is a config
value and not an assumption. It is settled for this panel; see "The LED mapping" in the README.

## Physical placement

From the parametric model in
[`enclosure/ws2812b_8x24_enclosure.py`](enclosure/ws2812b_8x24_enclosure.py).
The case is 205.6 mm long and stands upright on its front edge.

| Part | Position | Notes |
| --- | --- | --- |
| Touch zones | x = 55.0, 102.8, 150.6 mm on the top edge | upright in pockets, pad facing a 1.5 mm skin, each under a 6 mm dimple |
| Encoder | x = 176.0 mm on the top edge | bare EC11 lying behind the top wall, shaft out through it |
| MPU-6050 | (102.8, 30.0) mm | flat on the compartment floor under board 2, inside a fence |

The accelerometer's axis convention is **not yet established**. The case stands
on its front edge, so gravity rests along an in-plane axis in normal use and
along the board normal when the case is laid flat. Which signed axis is which
has to be measured on the real board before "lay it flat to sleep" can work.

## Joining wires

Several grounds and several 5 V feeds meet at the same points. Solder them
together and cover the joint with heat-shrink rather than using a terminal
block — there is no room in the compartment for one, and the model's wire
channels are sized for bare wire.
