[English](README.md) · [Русский](README.ru.md) · [简体中文](README.zh.md)

# KeySidekick

> Turn any spare keyboard into a dedicated background assistant — control media players, DAWs, OBS, and more without losing focus on what you're doing.

[![License: GPL-3.0](https://img.shields.io/badge/License-GPL--3.0-blue.svg)](LICENSE)
[![Platform: Windows](https://img.shields.io/badge/Platform-Windows%2010%2B-blue.svg)](#system-requirements)
[![Language: C++](https://img.shields.io/badge/Language-C%2B%2B14-orange.svg)](#building-from-source)

<p align="center"><img src="images/preview.webp" alt="KeySidekick — a spare keyboard becomes your control pad" width="100%"></p>

A second keyboard (or numpad, or macro pad) becomes a loyal **sidekick** — a dedicated controller for one or more apps that runs in the background and never steals focus. The main keyboard keeps working normally. Switch what the sidekick controls on the fly via the system tray, dedicated keys, or the built-in web dashboard.

---

## The problem this solves

You want one specific keyboard to control one specific application **without** its keys reaching the active foreground window — and you want to switch which app it controls at runtime.

This sounds simple but Windows fights you at every step:

| Approach | Distinguishes keyboards | Blocks keys | Lag |
|---|---|---|---|
| `WH_KEYBOARD_LL` hook | ❌ no device ID | ✅ yes | ❌ blocks the whole system input thread |
| Raw Input (`WM_INPUT`) | ⚠️ only if `hDevice != NULL` | ❌ read-only | ✅ none |
| Interception (kernel filter) | ✅ | ✅ | ✅ | but kernel driver, **hibernate lockup risk** |

And there's a nasty trap: **cheap / composite keyboards (keyboard+touchpad combos) report `hDevice == NULL`** in Raw Input, so even Raw Input can't tell them apart from the main keyboard — both look anonymous. The hybrid "Raw Input to identify + LL hook to block" then lags the whole system or produces false positives.

## The solution

**WinUSB Driver-Replacement Bypass.** The keyboard's interface is detached from the system keyboard stack (replaced with Microsoft's built-in `WinUSB.sys` via [Zadig](https://zadig.akeo.ie/)). Its keys no longer enter the Windows input queue at all — they can't reach any window. KeySidekick reads the raw HID reports directly via WinUSB and dispatches them according to the active **profile**.

This sidesteps every limitation above:
- ✅ No LL hook, so no system lag
- ✅ Works even when `hDevice == NULL` (we read the device directly, not via Raw Input)
- ✅ No kernel filter driver — just Microsoft's WHQL-signed WinUSB
- ✅ Survives hibernate/sleep (WinUSB handles ACPI natively)

See [`docs/PROBLEM-AND-SOLUTION.md`](docs/PROBLEM-AND-SOLUTION.md) for the full technical deep-dive.

---

## Features

- **Profiles** — multiple configurations, switch between them at runtime
- **Two modes per profile:**
  - `basic` — the dedicated keyboard **types normally** into the focused window (via `SendInput` re-injection). Useful when you temporarily want it to act like a normal keyboard.
  - `targeted` — keys are routed to a chosen app's window via `PostMessage`, never reaching the foreground; single-key mappings preserve key-down, Windows-rate repeat, and key-up
- **Multi-app routing** — within one profile, different keys can target *different* apps (`!app:Spotify:{Media_Play_Pause}` sends to Spotify while the rest of the profile goes elsewhere)
- **Key combinations** — `Ctrl+Shift+1`, `Alt+Q`, etc. as triggers (e.g. `USAGE_1E+Ctrl+Shift=!switch:aimp`)
- **Web Dashboard** — built-in control panel at `http://127.0.0.1:8765/`:
  - Create, rename, duplicate, delete profiles (no INI editing needed)
  - Visual application picker — select target from running windows
  - Action picker with chips (keys, media, switch, launch, multi-app)
  - Diagnostics page (device/driver/config health, recent log)
  - Help/Setup page (onboarding, troubleshooting, driver rollback)
  - Live updates via SSE (no manual refresh needed)
- **Ways to switch profiles, live:**
  - **Keys** on the dedicated keyboard (`!switch:`/`!toggle:` action tokens)
  - **System tray icon** — left-click → dashboard; right-click → menu with profiles and mode indicators
  - **Local HTTP API** on `127.0.0.1` — CSRF-protected, SSE-enabled
- **Reliability:**
  - Always-on device loop — app stays alive even if keyboard disconnected; event-driven idle (no polling, zero disk/CPU churn while nothing happens)
  - Singleton mutex — no duplicate instances
  - Injection ownership ledger — releases only keys it injected, never main-keyboard modifiers
  - Atomic config write — temp → validate → replace with backup
  - Hibernate/sleep safe — auto-reconnects on resume
  - Auto-launch target apps when their window isn't found
- **Auto-start with Windows** — managed via dashboard or API

## Known limitations (read before use)

- **Basic mode does not work in games / anti-cheat.** `SendInput` marks events with `LLKHF_INJECTED`; games like CS:GO, Valorant, EAC-protected titles detect and ignore (or ban) injected input. Basic mode is fine for browsers, editors, office apps, terminals. If you need game compatibility, the keyboard must stay on the native HID driver (don't use this tool's basic mode — use a `targeted` profile, or revert the driver via Zadig).
- **Targeted hold works only for message-driven apps.** Background hold/repeat is delivered as `WM_KEYDOWN`/`WM_KEYUP`. Apps polling `GetAsyncKeyState`, DirectInput, or Raw Input may ignore it.
- **Windows 10 (1809+) only.** Uses WinUSB + modern SetupAPI.
- **One-time Zadig setup required** — replaces the keyboard interface's driver (reversible; see [ZADIG_INSTRUCTIONS.md](ZADIG_INSTRUCTIONS.md)).

---

## Download / Install

1. Download the latest release ZIP (named `KeySidekick-<version>.zip`) from the project's Releases page on GitHub.
2. **Verify the SHA256 checksum** of the zip against the value published with the release.
3. Extract the archive anywhere — no installation required — and run `run.bat` (or `sidekick.exe` directly from the extracted folder).
4. Open http://127.0.0.1:8765/ in your browser.
5. Use **+ Setup keyboard** for the one-time Zadig driver swap, then **+ Pad template** to create your first control pad.

See [Quickstart](#quickstart) for the full walkthrough.

---

## Quickstart

### 1. Get the binary

Download the latest release zip from the [Releases](#download--install) page, **or** build from source (below).

### 2. Find your keyboard & swap the driver (one-time)

Open the dashboard (`http://127.0.0.1:8765/`) → **+ Setup keyboard**. Before Zadig your keyboard is an **ordinary** keyboard, and the wizard starts from there:

1. Lists **all** input devices — keyboards, mice, smart devices with keyboards — regardless of driver, with their state (`Normal keyboard (HidUsb)` vs `WinUSB — ready`).
2. **Press a key to identify** which VID/PID your keyboard is (Raw Input, works on ordinary keyboards too). For composite devices Windows may not expose per-key identity — then pick by name / VID / PID in the list.
3. **Preparation before the swap**: verify the keyboard types, enable KeySidekick auto-start (**after the swap the keyboard only types while sidekick.exe runs**), keep a fallback input, and confirm MI_00 is the keyboard (do *not* touch MI_01 — that's the mouse/touchpad).
4. Step-by-step **Zadig** guide for your exact VID/PID — the wizard watches for the device to flip to WinUSB and warns about the phantom-copy gotcha (`Reinstall Driver` if it still types normally).
5. **Verify** KeySidekick captures keys, then make the keyboard active (writes `DeviceVIDPID` into config).

Manual Zadig equivalent:

1. Download **Zadig** from <https://zadig.akeo.ie/> and run it **as administrator**.
2. **Options → List All Devices** ✓ (check it).
3. Find your keyboard's `MI_00` (keyboard) interface — e.g. `USB Input Device (VID xxxx PID yyyy) [MI 00]`. **Do not** pick `MI_01` (that's often a touchpad/mouse).
4. Target driver: **WinUSB (Microsoft)** (use the arrow buttons).
5. Click **Replace Driver** → wait for "SUCCESS".

Full steps + how to avoid breaking the wrong interface + rollback: [`ZADIG_INSTRUCTIONS.md`](ZADIG_INSTRUCTIONS.md).

### 3. Configure

Copy `src/config.example.ini` to `src/config.ini` and edit. Minimum (current **v3 schema** — `SchemaVersion=3`, profiles under `[Profile.<name>]`):

```ini
[General]
SchemaVersion=3
DeviceVIDPID=vid_xxxx&pid_yyyy     ; your device (see Zadig / Device Manager)
DefaultProfileId=basic
HTTPPort=8765
HTTPEnabled=1
TrayEnabled=1
EnableLog=1

[Application.aimp]
Name=AIMP
TargetClass=TAIMPMainForm           ; window class of the target app
TargetExe=AIMP.exe
TargetPath=C:\\Program Files (x86)\\AIMP\\AIMP.exe
AutoStart=1

[Profile.aimp]
Mode=targeted
ApplicationId=aimp                  ; link to [Application.aimp] above

[Profile.aimp.Mappings]
USAGE_14={F1}                       ; physical Q → F1
USAGE_1E=1                          ; physical 1 → 1
USAGE_29=!switch:basic              ; Esc → switch to basic (types normally)
USAGE_1E+Ctrl+Shift=!switch:aimp    ; Ctrl+Shift+1 → switch to aimp
```

Find your device's VID/PID in Zadig or via `Get-PnpDevice -Class Keyboard`. Find a target app's window class with [Spy++](https://learn.microsoft.com/en-us/visualstudio/debugger/introducing-spy-increment) or PowerShell (see [FAQ](docs/FAQ.md)).

### 4. Run

```
cd src
run.bat
```

Or directly: `sidekick.exe` (from the directory containing `config.ini`).

### 5. Control it

- **Web Dashboard** — open `http://127.0.0.1:8765/` in your browser. Left-click the tray icon to open it.
  - Create, rename, duplicate, delete profiles (no INI editing needed)
  - **+ Agent pad** — one-click preset that turns a spare keyboard into an AI-coding control pad (Codex / Claude / Cursor / Devin / ChatGPT): accept, cancel, branch, sidebar, voice, prompt-history, media — like a Stream Deck / Codex Micro, but mapped to your existing keyboard.
  - **Pad templates (use-case)** — ready-made F1–F12 profiles for media, OBS, meetings, PowerPoint, **REAPER**, **DaVinci Resolve**, **Ableton Live**, **Adobe Premiere**, **Lightroom Classic** (DAW/video hotkeys verified against official manuals).
  - **Live** — grid of the active profile's keys + a live feed of which actions just fired. Click any cell or any feed hit to **fire the action instantly** (`POST /api/v1/action/fire`) — no physical key needed.
  - **Typed Action Builder** — modal action editor (replaces browser `prompt()`): key/media/macro/switch/launch/send-to-app chips, live preview, picking running windows.
  - **First-run onboarding** — empty dashboard suggests three paths (Pad template / Create profile / Keyboard setup); «Start here» returns to it anytime.
  - **Macros** tab in the action picker — named combo/sequence examples (`{Ctrl+B}`, `{Ctrl+M}`, `{/}{Enter}`, …). Combos and multi-key sequences are supported as actions: `{Ctrl+Shift+F}`, `{Up}{Enter}`.
  - Visual application picker from running windows
  - Diagnostics and Help/Setup pages
  - Live updates via SSE (no manual refresh)
- **Tray icon** — left-click → open dashboard; right-click → context menu with profiles and mode indicators.
- **HTTP API** (loopback only, CSRF-protected):
  ```bash
  curl http://127.0.0.1:8765/api/profiles        # list profiles
  curl http://127.0.0.1:8765/api/v1/state        # unified state snapshot
  curl http://127.0.0.1:8765/api/v1/diagnostics  # health check
  curl http://127.0.0.1:8765/api/v1/hid          # all input devices + driver state (ready / needs-driver / ordinary)
  curl http://127.0.0.1:8765/api/v1/input/identify   # last keypress source (Raw Input; POST resets first)
  curl http://127.0.0.1:8765/api/v1/presets     # AI-agent pad catalog
  curl http://127.0.0.1:8765/api/v1/activity    # recent fired actions (Live screen)
  curl -X POST http://127.0.0.1:8765/api/v1/action/fire \
    -H "Content-Type: application/json" -H "X-KeySidekick-Token: $TOKEN" \
    -d '{"action":"{Media_Play_Pause}","usage":42,"profile":""}'  # fire an action like a pressed key
  ```
  Switch profiles / activate a device / apply an agent preset (requires CSRF token from dashboard):
  ```bash
  TOKEN=$(curl -s http://127.0.0.1:8765/ | grep -o 'CSRF_TOKEN="[^"]*"' | grep -o '"[^"]*"' | tr -d '"')
  curl -X POST http://127.0.0.1:8765/api/profile/activate -H "Content-Type: application/json" -H "X-KeySidekick-Token: $TOKEN" -d '{"name":"aimp"}'
  curl -X POST http://127.0.0.1:8765/api/v1/devices/activate -H "Content-Type: application/json" -H "X-KeySidekick-Token: $TOKEN" -d '{"vidpid":"vid_xxxx&pid_yyyy"}'
  curl -X POST http://127.0.0.1:8765/api/v1/preset/apply -H "Content-Type: application/json" -H "X-KeySidekick-Token: $TOKEN" -d '{"agentId":"codex","name":"Codex pad"}'
  ```
  > **For developers:** `tests/http_integration_tests.sh` runs against a live instance; it snapshots `src/config.ini` and restores it automatically on exit — no manual backup needed.
- **Keys** — whatever you mapped to `!switch:`/`!toggle:` in the active profile.

---

## Building from source

Requires [MinGW-w64](https://www.mingw-w64.org/) g++ (e.g. from [MSYS2](https://www.msys2.org/) or a standalone build). No Windows SDK paths needed — MinGW ships its own `winusb.h`/`setupapi.h`.

```
cd src
build.bat
```

`src\build.bat` performs the full build: it regenerates the embedded web dashboard from `web/` via `web/generate_dashboard.ps1`, compiles the icon resources with `windres` (`resources.rc` → `resources.o`), then links `sidekick.exe` (all 11 C++ sources + `resources.o`, WinUSB/SetupAPI/user32/ws2_32/… libs) and `probe_device.exe`. It looks for `C:\MinGW64\bin\g++.exe` first and falls back to `g++` from your PATH — either works as long as MinGW-w64 g++ is reachable.

`probe_device.exe` is a diagnostic tool — run it after the Zadig swap to dump the device's interface GUID, endpoints, and HID report descriptor (confirms the standard 8-byte keyboard report).

## System requirements

- Windows 10 version 1809 or later (x64)
- One free USB keyboard whose `MI_00` interface you're willing to put on the WinUSB driver
- [Zadig](https://zadig.akeo.ie/) for the one-time driver swap
- MinGW-w64 g++ (only if building from source)

## Documentation

- [`presentation.html`](presentation.html) — standalone trilingual landing page (EN/РУС/中文): problem, solution, features, pad templates, API
- [`ZADIG_INSTRUCTIONS.md`](ZADIG_INSTRUCTIONS.md) — the one-time driver swap, step by step (including how to undo it)
- [`docs/PROBLEM-AND-SOLUTION.md`](docs/PROBLEM-AND-SOLUTION.md) — the technical story: Windows input architecture, why `hDevice == NULL`, why the LL hook lags, why Interception is risky, why WinUSB wins
- [`docs/FAQ.md`](docs/FAQ.md) — common problems and fixes
- [`docs/HID-USAGE-TABLE.md`](docs/HID-USAGE-TABLE.md) — the HID Keyboard/Keypad Usage ID table for your INI mappings

## How it works (one paragraph)

The keyboard's HID interface is rebound from `hidusb.sys → kbdhid.sys → kbdclass.sys` to Microsoft's `WinUSB.sys`. With no keyboard-class driver bound, its keys never enter the Windows input queue — they're invisible to the foreground. KeySidekick (`sidekick.exe`) opens the device via the WinUSB API, asynchronously reads the 8-byte HID keyboard reports from the interrupt IN endpoint, diffs key-down/key-up state, looks up the active profile, and dispatches each key either as a one-shot action (`!switch`, `!app:`, ...) or as a held key lifecycle sent to the target window via `PostMessage` (`targeted` mode), or re-injects it into the system input stream via `SendInput` (`basic` mode). Targeted single-key mappings use the Windows keyboard delay/speed for repeat and retain the original target until physical key-up. Profiles, the active profile, the tray icon, and the HTTP server all share one message pump.

## Acknowledgements

- **[Zadig](https://zadig.akeo.ie/)** by [Pete Batard / Akeo](https://github.com/pbatard/libwdi) — the driver-swap tool that makes the WinUSB setup a one-click affair (LGPL).
- **Microsoft WinUSB** (`winusb.sys`) — the WHQL-signed user-mode USB driver this whole approach rests on.
- The **[USB HID Usage Tables](https://usb.org/sites/default/files/hut1_22.pdf)** (USB-IF) — the canonical source for the keyboard Usage IDs.

## Contributing

Issues and pull requests welcome. Please open an issue first to discuss bigger changes. Build with `src/build.bat` and confirm the [checklist](#building-from-source) before submitting a PR.

## License

Copyright © 2026. Licensed under the **[GPL-3.0](LICENSE)**.

This project uses but does not bundle [Zadig](https://zadig.akeo.ie/) (LGPL) — users download it separately. The router links against Microsoft Windows system libraries (`winusb`, `setupapi`, `ws2_32`, etc.) which are not covered by this license.
