# Pixelbar Mac helper

A menu-bar agent that sets the panel's status from what your Mac is doing.

| It notices | The panel shows |
| --- | --- |
| Microphone in use | `BUSY` |
| Camera in use | `CALL` — video beats audio, because it is the more useful thing to tell the room |
| Both idle again | whatever it was showing before, not `FREE` |

That last row is the one worth explaining. If you set `LUNCH` by hand and a
notification chime opens the microphone for two seconds, restoring to `FREE`
would quietly undo you. The helper remembers what it displaced and puts that
back.

## Build and install

```bash
make            # builds Pixelbar.app here
make run        # runs it without installing
make install    # copies to ~/Applications and starts it at login
make uninstall
```

No Xcode project, no signing, no developer account. `swiftc`, `make`, and a
Mac — nothing else. `~/Applications` rather than `/Applications` because it
needs no admin rights and belongs to one user.

On first launch it asks for the panel's address — the one the panel shows for
twelve seconds after it joins your network, or `GET /api/state` on any address
you already know. It lives in `UserDefaults`, so change it any time from
**Panel address…**.

## Permissions

**None.** It never asks for microphone or camera access, and could not use them
if it had them.

The distinction is the whole reason this approach is worth having: CoreAudio's
`kAudioDevicePropertyDeviceIsRunningSomewhere` answers *is some process running
this device*, which is a different question from *what is that process
recording*. A helper that had to request microphone access in order to notice
the microphone was busy would be a worse trade than the feature is worth.

## How it watches

The microphone is a **property listener**, not a poll — CoreAudio calls the
helper when the state changes, so a call starting is noticed at once and a
quiet afternoon costs nothing.

Only devices with input channels are watched. Without that filter your speakers
and your monitor count as audio devices that are "running" whenever anything
plays a sound, and a notification chime would look like a conference call.

The camera has no equivalent notification, so it is polled every five seconds —
which is fine, because a camera is nearly always on *because* the microphone
already is, and the microphone is the fast path.

**Two seconds of quiet** are required before the panel is handed back. Some
conferencing apps close the microphone between sentences, and a panel flicking
between `BUSY` and `FREE` would be worse than one that lags a little.

## The menu

- **Pause** — stop touching the panel, and hand back whatever was displaced.
- **Microphone sets BUSY** / **Camera sets CALL** — either rule, off.
- **Set status** — all eight, by hand, for the things no sensor knows about.
- The first line reads `Watching`, `Showing Busy`, `Paused`, or
  `Panel unreachable`.

## If the panel is unreachable

Nothing happens, loudly enough to see in the menu and quietly enough to ignore.
Requests time out in two seconds; a gadget that is switched off is an ordinary
condition, not an error worth a dialog.
