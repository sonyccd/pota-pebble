# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & deploy

```sh
pebble build                        # build for all targetPlatforms
pebble install --emulator emery     # run on the emery (Pebble Time 2) emulator
pebble install --phone <ip>         # sideload to a paired phone over Wi-Fi
pebble logs --emulator emery        # stream APP_LOG output from the emulator
```

There is no test framework — manual testing via the emulator is the primary verification path.

## Architecture

This is a Pebble SDK 3 watchapp written in C. The entry point is `src/c/pota-pebble.c`.

The Pebble event-loop model:
- `main()` calls `prv_init()`, then `app_event_loop()` (blocks until app exits), then `prv_deinit()`.
- UI is built from `Window` and `Layer` objects. Layers are created in a `window_load` handler and destroyed in `window_unload`.
- Button input is wired via a `ClickConfigProvider` registered on the window.
- Phone-side JavaScript lives in `src/pkjs/index.js` (PebbleKit JS); it communicates with the watch via `AppMessage`.

Platform targets are declared in `package.json` under `pebble.targetPlatforms`. The build system (`wscript`, using Waf + `pebble_sdk`) compiles a separate ELF per platform and bundles them into a single `.pbw`. Background worker code goes in `worker_src/c/` — the build detects this directory automatically.

## SDK reference

Full API docs: <https://developer.repebble.com>
