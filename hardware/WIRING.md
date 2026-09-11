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
| Encoder `A` | 20 | 10 kΩ pull-up to 3V3 |
| Encoder `B` | 21 | 10 kΩ pull-up to 3V3 |
| Encoder switch | 0 | to ground; 10 kΩ pull-up to 3V3 |

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
| `+` (or `5V`) | **3V3** | never 5 V — see below |

### Connect `+` to 3.3 V, and never to 5 V

**The ESP32-C3's GPIOs are 3.3 V and are not 5 V tolerant.** The module's `+`
pin feeds 10 kΩ pull-up resistors that sit on `S1`, `S2` and `KEY`, so wiring
it to 5 V drives 5 V straight into three GPIOs through those resistors. Use
the board's 3V3 pin.

An earlier version of this file said to leave `+` unconnected, on the
reasoning that the ESP32-C3's internal pull-ups make the module's own
redundant. That was wrong, and wrong in a way that took a while to see.

With `+` floating, those three resistors do not disappear — they tie `S1`,
`S2` and `KEY` together through an undriven node. The internal pull-ups are
about 45 kΩ against the module's 10 kΩ, so the lines end up weakly held and
coupled to each other rather than cleanly high. Two things follow, and both
were observed on the bench:

- **Phantom input while nothing is being touched.** Twelve seconds of sitting
  perfectly still produced 32 illegal quadrature transitions, a detent that
  nobody turned, and a switch press that started the timer. The panel appeared
  to navigate and change status by itself.
- **The switch dipping whenever the knob turns.** Diagnosed at first as the
  push switch sharing a ground with the rotary contacts. It is really this
  coupling: working the rotary contacts disturbs the shared floating node, and
  `KEY` moves with it. The firmware carries two guards against this — it will
  not read the switch while the knob is moving, and it rejects any contact
  shorter than a finger can make — but they are mitigations for a wiring
  fault, not a substitute for fixing it.

Powering `+` from 3V3 puts strong 10 kΩ pull-ups on all three lines and makes
the coupling node a supply rail instead of an antenna.

## The bare EC11, and the three resistors it needs

This is the final build: the encoder desoldered from its breakout board, or
bought bare. A bare EC11 is the switch and the two contacts and nothing else —
five pins, no power pin, and **no resistors**. There is no `+` to connect and
nothing is lost by its absence: the shared node that caused the fault above is
removed along with the board it lived on.

| EC11 pin | Goes to | |
| --- | --- | --- |
| `A` | **GPIO20** | quadrature A, **and** 10 kΩ to 3V3 |
| `C` (common, centre of the three) | GND | |
| `B` | **GPIO21** | quadrature B, **and** 10 kΩ to 3V3 |
| switch pin 1 | **GPIO0** | **and** 10 kΩ to 3V3 |
| switch pin 2 | GND | |

### Fit the three 10 kΩ pull-ups

One from each of `A`, `B` and the switch pin to **3V3**. Not optional, and not
the same thing as the module's resistors — those are gone with the module.

The firmware enables the ESP32-C3's internal pull-ups, and on a bench with
short wires those alone will appear to work. They are around 45 kΩ, which is
weak enough that the line sits at a high impedance between contact closures,
and a high-impedance line a few centimetres from 192 WS2812s switching at
100 Hz is an antenna. The wires are also longer in the assembled case than on
the bench, which makes it worse exactly where it is hardest to get at.

This is a different mechanism from the coupling fault above — pickup on a weak
pull-up, rather than three lines tied through an undriven node — and it shares
the same symptom, which is input nobody made. 10 kΩ lowers the impedance by
more than four times and settles it. Three resistors, fitted before the case
closes, against a fault that otherwise surfaces only after it has.

The switch pull-up matters as much as the two quadrature ones. The firmware
carries guards against a disturbed switch line — it will not read the switch
while the knob is moving, and it rejects any contact shorter than a finger can
make — and those guards cost responsiveness. They exist to survive bad
signals, not to excuse them.

### If the direction comes out backwards

Swap the two quadrature lines — `S1` and `S2` on the module, `A` and `B` on a
bare EC11. Which line is which only sets the sense of rotation, and getting it
the wrong way round is the expected outcome of a coin flip rather than a
mistake.

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
