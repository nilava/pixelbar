# Contributing

Patches welcome. A few things worth knowing before you start, because they are
constraints rather than preferences and the build enforces most of them.

## Run the tests

```bash
./test/run.sh
```

No ESP-IDF, no hardware, a few seconds. If this is red, nothing else matters.

## The two layers that must stay portable

`firmware/components/panel` (drawing) and `firmware/components/ui` (events,
gestures, state, settings) **must not include an ESP-IDF header**. `test/run.sh`
compiles both with plain `g++`; adding a source either of them that only builds
under the toolchain breaks that script, which is exactly what it is for.

Anything needing a peripheral goes behind `ui::Ports` — implemented by
`components/board` on the device and by `tools/sim.cpp` in memory.

`components/net` deliberately does not `REQUIRES panel ui`. Nothing there may
touch the model; commands cross to the render loop through a queue.

## Write the test so that it can fail

The most useful habit in this repo: after fixing something, **revert the fix
and check the test goes red**. It has caught a test that swept seven
milliseconds of a six-second period and passed against the very bug it was
written for. A test you have not seen fail is a test you have no reason to
believe.

## Measure labels, do not eyeball them

The mini font gives `M` and `W` five columns and most letters three, so a
four-letter label containing either measures 17 against a box 15 wide. There is
a test over every label in the settings tree; if you add one, it will tell you.

## Never commit a credential

There is no credentials file and there should never be one again — see the note
in the README about where a password in a header actually ends up. Provisioning
is the only way in.

## Commits

Explain *why*, and say what you tried that did not work. Several comments in
this tree document a wrong answer that looked right for a week; those are worth
more than the code around them.
