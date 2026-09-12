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

No Xcode project, no developer account, no certificate. `swiftc`, `make`, and a
Mac — nothing else. `~/Applications` rather than `/Applications` because it
needs no admin rights and belongs to one user.

The build does **ad-hoc sign** the bundle, and that is not about distribution.
macOS attributes the Local Network permission to a code identity, and `swiftc`
leaves a linker-signed binary whose identifier is `Pixelbar` rather than the
bundle id — which TCC cannot hold a durable grant against. The symptom is
precise and thoroughly misleading: **discovery finds nothing when launchd
starts the app at login, and the identical binary works when you run it from a
terminal**, because there it inherits the terminal's own permission. Signing
with `--identifier com.pixelbar.helper` is the fix, and it needs no account.

On first launch it **finds the panel by itself**. There is nothing to type.

If it cannot — and there are networks where it cannot — **Find panel** retries
and reports what happened, and **Panel address…** still takes one by hand.

## Permissions

**None.** It never asks for microphone or camera access, and could not use them
if it had them.

The distinction is the whole reason this approach is worth having: CoreAudio's
`kAudioDevicePropertyDeviceIsRunningSomewhere` answers *is some process running
this device*, which is a different question from *what is that process
recording*. A helper that had to request microphone access in order to notice
the microphone was busy would be a worse trade than the feature is worth.

## Your calendar

**Off by default** — reading somebody's calendar is a thing to opt into, not a
thing to discover has been happening. Switch on **Calendar sets MEET**.

| When | What happens |
| --- | --- |
| A meeting starts in the next ten minutes | its title scrolls on the panel with a live countdown |
| A meeting is running | the panel shows `MEET` |
| Your microphone or camera is live | that wins — see below |

It reads through **EventKit**, which means whatever accounts Calendar.app
already has: iCloud, Google, Exchange, a subscribed ICS. No OAuth client, no
consent screen, no client secret in a repo, no refresh token to expire at the
worst possible moment. One permission prompt on first run, and it works offline
against the local store. The cost is that an account not added to Calendar.app
is invisible here — a fair trade for not implementing an identity provider
inside a desk ornament.

**The sensors outrank the calendar**, in this order: camera → `CALL`,
microphone → `BUSY`, meeting in progress → `MEET`. That ordering is by how
directly each thing is observed. A live camera is happening now; a calendar
entry is somebody's earlier intention, and people leave early, join late, and
decline by walking away.

Declined invitations are skipped, all-day events are ignored, and a meeting is
announced only in its last ten minutes — earlier than that it is not news, and
a panel showing the same meeting for an hour is a panel nobody looks at. The
announcement is sent once per meeting rather than on every poll, because
re-sending would restart the scroll.

An event counts as a video call if a known joining host appears in its URL,
location or notes — Meet, Zoom, Teams, Webex, Whereby, Jitsi. Deliberately a
short list rather than any URL: "there is a link in the notes" is true of most
meetings and means nothing.

## Two ways to reach the panel

**Wi-Fi first, Bluetooth second.** Not really a preference — an ordering by
capability. Wi-Fi is the pipe that also carries firmware images and the web
page, so when it is there it is the one to use. Bluetooth needs no credentials,
no router, no address and no discovery at all, which is exactly the situation
in which it is the only one left: a panel that has never been provisioned, or
one whose network is down, or a network that blocks traffic between two clients.

The menu says which one is carrying commands.

Both speak the same JSON. A write to the Bluetooth command characteristic and a
`POST /api/input` are the same input in the same words, parsed by the same
function on the device — two transports that disagreed slightly about what a
command meant would be a bug nobody finds until the day one of them is all
there is.

## Setting it up

The menu shows **Set up Pixelbar…** and nothing else until it is paired. Not
greyed-out options with no explanation — one obvious next step, and a first line
that says which part is missing: no panel found, not paired, or paired but
unreachable. Three different problems with three different remedies.

Setup finds the panel, asks it to show a six-digit code, and takes that code in
exchange for a token. Until that is done the helper sends nothing, so an
unpaired helper is idle rather than silently failing every time a microphone
opens.

## Bluetooth pairing

**The panel shows a six-digit code and macOS asks you to type it.**

That is not ceremony. Without it, anything within about ten metres could drive
the panel — a different and worse posture than the HTTP API, because a home LAN
at least has a door on it. Bluetooth's own answer to this is "Just Works"
bonding, which encrypts the link against eavesdroppers and authenticates
nobody: anything in range can bond, and is then trusted for good.

A passkey fixes the half that matters. The code is generated on the device from
its hardware random number generator, exists nowhere else, and is displayed on
a panel you have to be looking at — so a host that can produce it is a host in
the room. The command characteristic requires an authenticated link, so there
is no way to drive the panel without having done this once.

Use **Pair over Bluetooth…** from the menu. It is deliberate rather than
automatic because the alternative is being asked to read six digits off a panel
at whatever moment your microphone first happens to open.

Reading the panel's state needs an encrypted link but not an authenticated one:
what it returns is what the panel is already showing the room, and charging a
pairing prompt to keep a secret that is painted on the wall would be theatre.

A host that bonded before and comes back with new keys — a Mac that was
re-imaged, say — is re-paired rather than refused, because the alternative is a
device nobody can reconnect to and nothing on it to clear the old bond from.

## How it finds the panel

Two ways, in this order, because neither works everywhere.

**A UDP broadcast probe** on port 51737. The panel answers with its name and
address. One packet out, one back; measured at about a second on a home
network, nearly all of which is the window held open in case a second panel
answers.

**A sweep of the local /24** if nothing replies. Some networks drop broadcast
between clients — "AP isolation", common on guest and mesh setups — and on
those the probe goes nowhere. Asking all 254 addresses for `/api/state` is
crude, and it works where broadcast does not. Measured at about nine seconds,
which is a fine price for a fallback and a poor one for a default, so it only
runs when the probe finds nothing.

More than one panel produces a chooser rather than a silent pick, because
picking silently would be picking wrongly half the time.

> **Why not mDNS?** It is the obvious answer and it costs more than it looks.
> mDNS is a managed component in ESP-IDF 5.x, and pulling it in would break the
> firmware's promise that it builds offline with nothing to download. A correct
> responder is also considerably more than this — PTR, SRV, TXT and A records,
> name conflict resolution, and politeness toward every other responder on the
> segment — to answer one question asked by one helper. The device already runs
> a hand-written DNS responder for its setup portal, so a second datagram
> handler was about fifty lines against a dependency.

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
