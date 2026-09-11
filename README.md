# Pixelbar

An 8×24 WS2812B desk panel driven by an ESP32-C3, built as an open alternative
to a Busy Bar: it shows your status to the room, runs a focus timer, and takes
input from three hidden touch zones and a rotary encoder.

The panel is three 65 mm 8×8 WS2812B boards butted edge to edge inside a
snap-fit printed case, behind a per-LED light grid and a diffuser.

![scrolling text](docs/preview/text.png)

## Status

**Step 1 of the firmware is done: the display layer.** Framebuffer, LED
mapping, proportional font, pattern set, gamma and the power cap are unit
tested on the host, and the ESP32-C3 image builds and is ready to flash.

None of it has run on real LEDs yet, because the boards are not built. The
first job once they are is the mapping calibration below.

Still to come, in order: inputs and the status state machine, the MPU-6050,
WiFi with a web page and OTA, the Mac helper that flips the panel to BUSY when
your microphone opens, then Slack and calendar.

## Hardware

| Part | Notes |
| --- | --- |
| 3 × 8×8 WS2812B matrix | 65 mm boards, 8 mm pitch, chained left to right |
| ESP32-C3 SuperMini | USB Serial/JTAG, so GPIO20/21 are free |
| 7Semi USB-C breakout | 5 V in, separate from the ESP32's own port |
| 3 × TTP223 (HW-763) | touch zones, sensing through 1.5 mm of the case wall |
| MPU-6050 (GY-521) | tap and orientation |
| EC11 encoder | bare, behind the top wall, knob on the top edge |
| 1N5819, 330 Ω, 1000 µF | back-feed block, data series resistor, bulk cap |

The full wiring diagram, GPIO map and wire list live in `hardware/`, along with
the parametric enclosure model and its interference audit.

```
hardware/enclosure/ws2812b_8x24_enclosure.py   # the model; edit params, re-run
hardware/enclosure/check.py                    # 16 interference + clearance checks
hardware/enclosure/fit_test.py                 # corner coupons for a test print
hardware/stl/                                  # tray.stl, grid.stl and the coupons
```

## Firmware quick start

Needs ESP-IDF v5.3 or newer. Nothing else: there are no managed components to
download, so it builds offline.

```bash
. $HOME/esp/esp-idf/export.sh
cd firmware
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

## Calibrating the LED mapping

How the LEDs are wired *inside* one 8×8 board varies between suppliers, so it
is a config value rather than an assumption. On first flash the panel runs the
mapping test for 24 seconds:

- a **red** pixel marks logical (0,0), the top-left corner
- a **white** pixel walks the panel in logical order
- **green** pixels on the bottom row mark the seams between boards

The white pixel should sweep left to right along the top row, then drop to the
next row. If it zig-zags, runs down columns, or starts in the wrong corner,
edit `kWiring` in `firmware/components/panel/include/panel/config.h` and
reflash. All sixteen combinations are covered by the tests, so any setting you
pick is guaranteed to address all 192 LEDs exactly once.

Once it is right, set `kMapTestSeconds` to 0 in `firmware/main/main.cpp`.

## Tests and preview, no hardware needed

The whole drawing layer is free of ESP-IDF, so it builds and runs on a laptop.

```bash
./test/run.sh                      # builds and runs the tests
./build-host/preview docs/preview  # renders every pattern
python3 tools/ppm2png.py docs/preview
```

The preview reads back the same GRB bytes that would go out on the wire,
through the same mapping, so a gamma or mapping mistake shows up on screen
instead of on a soldered panel.

| | |
| --- | --- |
| ![clock](docs/preview/clock.png) | ![plasma](docs/preview/plasma.png) |

## Power

192 LEDs at full white would draw about 11 A, which no USB-C supply will give
you. `Framebuffer::render` estimates the draw of every frame and scales it to
stay under `kMaxMilliamps`, defaulting to 2500 mA on a 3 A supply. The cap is
enforced in the render path, not in the patterns, so no pattern can exceed it.

## Layout

```
firmware/components/panel/   framebuffer, mapping, font, patterns (no IDF deps)
firmware/components/ws2812/  WS2812B over RMT, no external components
firmware/main/               app_main, GPIO map
test/                        host-side tests
tools/                       PNG preview renderer
hardware/                    enclosure model, STLs, wiring
```

## License

MIT, see `LICENSE`.
