# Pixelbar from your phone

There is no app, and there is not going to be one. iOS Shortcuts can send an
HTTP request, the panel has an HTTP API, and an app would need a developer
account to survive more than seven days on your own phone.

So the phone talks to the panel the same way the Mac helper does: a POST with
an `X-API-Token` header. What Shortcuts adds is *triggers* — an NFC tag on your
desk, a Focus mode turning on, a time of day, a button on your Home Screen.

> Shortcuts cannot speak Bluetooth GATT, so this is the Wi-Fi path only. On a
> network with client isolation — many office and guest networks — your phone
> cannot reach the panel at all, and nothing here will work. That is the case
> Bluetooth exists for, and it is the Mac helper's job.

## Get a token

One per device, so you can revoke the phone without disturbing the Mac.

1. On the panel: hold the knob → **HOST** → **ADD**. A six-digit code appears.
2. Open `http://<panel-address>/` on the phone, enter the code, and give it a
   name. You get a 32-character token back.
3. The panel's address is on **WIFI → IP**, and the paired list is
   **HOST → LIST**.

Keep the token somewhere the Shortcut can read it. A text field in the Shortcut
itself is fine for a device you carry; it is not a password to anything else,
and **HOST → DROP** on the panel revokes every one of them.

## The one Shortcut everything else is a variation of

**Get Contents of URL**

| Field | Value |
| --- | --- |
| URL | `http://192.168.68.105/api/input` |
| Method | `POST` |
| Headers | `X-API-Token` → your token |
| Request Body | JSON |
| JSON | `status` (Number) → `1` |

That is a whole recipe. Everything below changes the body.

## Set a status

`POST /api/input` with `{"status": n}`:

| n | Status | n | Status |
| --- | --- | --- | --- |
| 0 | FREE | 4 | AWAY |
| 1 | BUSY | 5 | FOCUS |
| 2 | CALL | 6 | LUNCH |
| 3 | DND | 7 | MEET |

**From a Focus mode.** Settings → Focus → *Do Not Disturb* → Add Automation →
Run Shortcut. One Shortcut sets `3`, and the "when Focus ends" one sets `0`.
This is the single most useful thing on this page: the panel then tells the
room what your phone already knows.

**From an NFC tag.** A sticker on the edge of your desk. Automation → NFC →
Scan. Tap to go BUSY on the way into a call, tap again on the way out — one
tag, with an *If* on the current status if you want it to toggle:

```
Get Contents of URL   http://<panel>/api/state        (GET, no token needed)
Get Dictionary Value  status
If  status  is  BUSY
    …POST {"status": 0}
Otherwise
    …POST {"status": 1}
```

Reads need no token, which is what makes that first step work.

## Put something on the panel

`POST /api/display/draw` — text, an icon, a countdown, a progress rail, or any
combination.

```json
{"text": "STANDUP", "icon": "timer", "ttl": 30, "color": "#FF8A1F"}
```

| Field | Meaning |
| --- | --- |
| `text` | up to 47 characters; it scrolls if it does not fit |
| `icon` | `free` `busy` `dnd` `moon` `sun` `timer` `gear` `grid` `display` `hand` `motion` `lock` `cross` `warning` `download` `info` `palette` |
| `color` | `#RRGGBB` |
| `ttl` | seconds; `0` means until something replaces it |
| `until` | a Unix timestamp — draws a **countdown** to it |
| `bar` | `0`–`100`, draws a progress rail |
| `priority` | `1`–`100`, default `50` |
| `source` | who is asking; the same source replaces rather than stacks |

`DELETE /api/display/draw` takes it away again.

**A countdown to your next meeting.** Shortcuts can read your calendar, so this
needs no permissions the phone has not already granted:

```
Find Calendar Events  where Start Date is after Current Date, limit 1
Format Date           <Start Date>  as Unix timestamp
Get Contents of URL   POST /api/display/draw
                      {"text": <Title>, "until": <timestamp>, "icon": "timer"}
```

**A leave-now alarm.** Run it at 17:25 on weekdays:

```json
{"text": "TRAIN", "until": 1789012345, "color": "#40C4FF", "priority": 80}
```

`priority` is what decides between two things wanting the panel at once. A
status claim is 50; anything above that wins.

## Drive the panel itself

`POST /api/input` also carries the controls, which is how the web page works:

| Body | Effect |
| --- | --- |
| `{"turn": 1}` | one detent clockwise; negative goes back |
| `{"press": 1}` | click the knob |
| `{"presshold": 1}` | hold it — opens the menu |
| `{"tap": 0}` | tap a pad: `0` left, `1` middle, `2` right |
| `{"swipe": 1}` | swipe across; negative goes the other way |
| `{"brightness": 40}` | `0`–`255` |

A pad tap and a finger arrive at the recogniser as the same thing, so anything
you can do standing at the desk you can do from the sofa.

## Check on it

`GET /api/state` needs no token:

```json
{"screen":"status","status":"FREE","brightness":48,"timer_left":1500,
 "timer_running":false,"fps":100.0,"ip":"192.168.68.105","clock":"18:48"}
```

`GET /api/clients` lists who is paired — names and dates, never tokens.

## When it stops working

**Everything returns 401.** The token was revoked, most likely by
**HOST → DROP** or a factory reset. Pair again.

**Nothing connects at all.** Check **WIFI → IP** on the panel. A DHCP lease
moves, and a Shortcut holding an old address will simply time out. Give the
panel a reservation on your router if you get tired of this.

**It works at home and not at the office.** Client isolation. Nothing in this
document can fix that; it is what the Mac helper's Bluetooth path is for.
