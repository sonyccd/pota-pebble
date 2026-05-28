# POTAPebble

A Pebble watchapp for [Parks on the Air (POTA)](https://parksontheair.com) hunters. Fetches live spots from the POTA API and displays them on your wrist — keep your phone in your bag and focus on the radio.

**KK4PWJ** — built for aplite, runs on all Pebble hardware.

## Features

- Live spot list, updated every 30 seconds
- Filter by band (160m–70cm) and mode (SSB, CW, FT8, FT4, RTTY, AM, FM, Digital)
- Vibrates once per poll cycle when new matching spots arrive (silent on first load)
- Tap any spot for full detail: park name, frequency, mode, comment, age
- Cursor follows the same spot across list refreshes
- "No phone" banner when Bluetooth drops; list freezes and resumes on reconnect
- Filters persist across app restarts

## Usage

1. Open the app → **Spots** to see the live list
2. Up/Down to scroll, Select to view spot detail, Back to return
3. From the main menu → **Settings → Bands / Modes** to toggle filters

## Building

Requires the [Rebble SDK](https://developer.repebble.com).

```sh
pebble build                        # build for all platforms
pebble install --emulator aplite    # run on the aplite emulator
pebble install --phone <ip>         # sideload to a paired phone
pebble logs --emulator aplite       # stream logs
```

## Architecture

| Layer | Responsibility |
|---|---|
| `src/c/pota-pebble.c` | Watch UI, navigation, vibration, persist mirror |
| `src/pkjs/index.js` | HTTP polling, filtering, diffing, settings (localStorage) |

The phone-side JS owns the canonical filter state in `localStorage`. The watch mirrors it in `persist_*` for instant UI rendering on launch, then PKJS overwrites the mirror via `SETTINGS_SYNC` on each startup.

Polling runs only while the Spots screen is visible. No background behavior.

## API

Uses the unofficial POTA API: `https://api.pota.app/spot/activator`

Polls every 30 seconds. Identifies as `POTAPebble/0.1 (KK4PWJ)`.
