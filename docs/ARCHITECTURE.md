# Architecture

How the firmware is arranged, and why. Most of what follows is a constraint of
the ESP32-C3 rather than a preference — this chip is missing several things the
obvious design would have used.

## What the chip does not have

| Missing | Consequence |
| --- | --- |
| **FPU** | `sinf` is emulated in software. Plasma alone wanted 576 of them per frame, so trigonometry is a 256-entry table. |
| **PCNT** | No hardware quadrature counter. The encoder is decoded in a GPIO interrupt on both edges of A and B, running a four-state table into an atomic `int32_t`. |
| **Touch peripheral** | Hence external TTP223 modules rather than capacitive pins. |
| **RMT DMA** | `SOC_RMT_SUPPORT_DMA` is defined for the S3 and absent here. See below — this one changed the whole output path. |

## Components

```
firmware/components/panel/   drawing.       IDF-free.
firmware/components/ui/      events, gestures, state, settings. IDF-free.
firmware/components/board/   GPIO, the encoder ISR, NVS.  IDF only.
firmware/components/net/     WiFi, portal, SNTP, HTTP.    IDF only.
firmware/components/ws2812/  the LED driver.              IDF only.
firmware/main/               wiring, and the render loop.
tools/sim.cpp                the terminal simulator.
```

`panel` and `ui` contain no ESP-IDF header. That is load-bearing: `test/run.sh`
compiles both with plain `g++` and runs 28,807 assertions with no toolchain and
no hardware, and those two layers are the ones most worth testing.

Everything needing a peripheral sits behind `ui::Ports` — a small abstract
struct with `read_raw`, `load_settings`/`save_settings`, `wall_clock`,
`wifi_connected` and `net_mode`. `board` implements it on the device; the
simulator implements it in memory.

> `board` is not called `hal`. ESP-IDF already has a component of that name, and
> a second one shadows its headers until `mbedtls` cannot find `sha_types.h`.

`net` deliberately does **not** `REQUIRES panel ui`. An HTTP handler runs on the
server's task and the model runs on the render loop; the only thing crossing
between them is a twelve-byte command through a FreeRTOS queue, drained at the
top of a frame. If someone later includes a model header there, that CMake line
is what fails.

## Why the LED output is SPI, not RMT

RMT is the obvious peripheral for WS2812B and it is the wrong one on this chip.
Without DMA it is refilled from an interrupt: 192 LEDs × 24 bits = 4,608 symbols
per frame, refilled 48 at a time, so **96 interrupts per frame, 9,600 a second**
at 100 fps, each with about 58 µs of runway. A WS2812B latches after roughly
50 µs of idle line, so one late refill latches a partial frame and restarts at
LED 0.

On the bench that was a flicker travelling along the panel, worse while the
encoder was being turned — the GPIO interrupt decoding it was competing for the
same microseconds. Widening the buffer and raising the interrupt priority made
the race easier to win. It did not stop it being a race, and WiFi is far less
polite than an encoder: `nvs_commit()` and `esp_ota_write()` disable the flash
cache for milliseconds at a time, while the ISR lives in flash. Saving a setting
would tear the panel.

So the deadline had to stop existing rather than get easier. **SPI2 + GDMA**:
four SPI bits per WS2812 bit at 3.2 MHz is exactly 1.25 µs per bit.

```
0 -> 1000    312 ns high, 937 ns low   (T0H spec 220-380 ns)
1 -> 1110    937 ns high, 312 ns low   (T1H spec 580-1000 ns)
```

The reset gap is more zero bytes on the end: 112 of them hold the line low for
280 µs. Once queued, DMA clocks the whole frame out of SRAM with no CPU and no
interrupt-latency exposure anywhere in it. Cost: the wire buffer grows from 576
to 2,416 bytes.

## Temporal dithering, and its one hard edge

At low brightness the gamma curve leaves only about 44 distinct levels, so a
four-second fade holds each for nine frames and visibly steps. A sigma-delta
error term carried between frames recovers the intermediate values.

It has a floor, and the floor is not a matter of taste. **The pulse rate is the
value**: a pixel at 1/8 of a level lights one frame in eight. Below roughly
30 Hz those pulses stop fusing and you see individual flashes. There is a hard
cutoff at `kDitherKnee = 256 * 30 / kFramesPerSecond` — below it the output is
zero and the error is cleared.

Three things were tried first and are worth recording because each seemed
obviously right:

- **Jitter alone** made the flicker less tonal but no less visible.
- **Jitter on dark pixels** produced stray lit LEDs — 132 strays over 600 frames.
- **A smooth roll-off made it worse**, dropping the pulse rate from 4.7 Hz to
  0.4 Hz. A rarer pulse is *more* noticeable, not less.

One more: the jitter value is shared across all three channels of a pixel.
Per-channel jitter meant the channels crossed the threshold on different frames,
so 50 frames in 600 had only one channel lit — a visible hue shift on what
should have been a neutral dim grey.

## The disk

The panel is read as a radial strip of a record: column 0 at the hub, column 23
at the rim. Content advances through the same *angle* at every radius and
therefore a different *distance*, so the rim end whips past while the hub end
crawls, and a glyph shears as it goes.

`blit_disk(dst, src, turn, gain_hub, gain_rim)` applies a vertical shear
proportional to radius, `dy(x) = turn * (kDiskHubRadius + x)`. The constants
matter: with a hub radius of 5 and a clear turn of 0.29 the hub moved only 1.4
rows and the whole thing read as a cross-fade. Hub radius 3 and a turn of 0.85
is what makes it read as rotation.

## Navigation

An explicit stack of depth four: Home → Menu → Group → Setting. Press goes in,
hold comes out.

Two contract details that cost an evening each if you get them wrong:

**Going back needs its own direction.** `kind_for(from, to)` is a pure function
of the pair, so Home→Menu and Menu→Home resolve identically. Each stack frame
records the kind it was pushed with and `pop()` plays the inverse, which makes
back-navigation correct by construction for any screen added later.

**The outgoing screen must be drawn with the state it had when the move began.**
`ScreenManager` used to snapshot the state lazily on the first transition frame,
but every caller mutates it *before* calling `go_to` — so the outgoing screen was
drawn with the incoming state. `go_to` and `restart_with` now take the outgoing
`UiState` explicitly. Reverting that makes 28 assertions fail.

**Model-side timers accumulate `dt`; they never compare against an absolute `t`.**
Otherwise pausing or time-dilating the simulator stretches the transitions but
not the timeouts.

## The encoder, and its shared ground

On a KY-040 breakout the push switch shares its ground net with the rotary
contacts, so turning the knob disturbs the switch line. Two guards, both of
which exist because of that and not because a switch needs them:

- The switch is **not read at all while the knob is moving**. A longer debounce
  cannot work: during a fast turn the contacts make and break continuously, so
  the disturbance lasts as long as the turning does.
- A contact shorter than 30 ms is not a press. The guard above only arms once a
  detent has been *decoded*, and a detent is four quadrature edges — tens of
  milliseconds after the knob really started moving, by which time a coupled dip
  has already latched and released. Nothing that looks backwards can fix that.
  What separates a finger from a bounce is duration, and since `Press` is
  emitted on release, the check costs no latency.

Both are mitigations for a wiring fault. See [WIRING.md](../hardware/WIRING.md):
the real fix is 10 kΩ pull-ups on `A`, `B` and `SW`.

An illegal quadrature transition — both lines changing between samples — is
counted and **not** used to discard the quarter-steps before it. Discarding them
was tried and was wrong twice over: it did not address the noise it was aimed at
(that drift produces perfectly legal Gray code), and at speed an illegal
transition means a *missed edge*, so throwing away three quarter-steps loses a
detent the user actually turned.
