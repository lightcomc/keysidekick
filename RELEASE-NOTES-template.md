# KeySidekick 0.9.5 — Release Notes

## What's new

- **Fixed: stuck Ctrl/Shift during device tests** — the wizard's detect/capture probes no longer steal input reports from the active keyboard (the root cause of injected modifiers staying pressed until unplug).

- **One file for everything** — the driver helper is now built into `sidekick.exe` (`--driver swap|restore|status`); the app itself never asks for admin rights, UAC appears only when you actually swap or restore the driver.
- **Portable + fully offline** — carry the folder on a USB stick; profiles travel in `config.ini`; the program makes zero network connections (loopback dashboard only, no telemetry).

- **Driver swap without Zadig** — KeySidekick now swaps the keyboard driver itself via the Microsoft-signed inbox `winusb.inf` (`ks_driver.exe`, UAC prompt), and restores the normal HID driver any time. Zadig remains as a manual fallback.
- **Port-change recovery** — plug the keyboard into another USB port and the dashboard offers a one-click "Apply driver again"; all profiles come back automatically (they are stored per VID/PID).

- **First-run onboarding** — a guided 3-path start screen (pad template / create
  profile / keyboard setup) for new installations.
- **Driver-swap wizard** — step-by-step Zadig driver replacement flow right from
  the dashboard, including risk disclosure and rollback guidance.
- **HID device list** — the dashboard enumerates every input device
  (keyboard/mouse/smart devices) with driver state: `ready` (WinUSB),
  `needs-driver`, or `ordinary`.
- **Keypress identification** — press a key and KeySidekick tells you which
  physical device (VID/PID/name) produced it, so setup never requires guessing.
- **AI-agent + use-case pad presets** — one-click pads for Codex, Claude,
  ChatGPT, Cursor, Devin, Copilot, media players, OBS, Meet, Office, and DAW
  suites (Reaper, DaVinci, Ableton, Premiere, Lightroom).
- **Combo macros** — modifier+key chords (e.g. `{Ctrl+B}`) as first-class
  mappings.
- **Live click-to-fire** — fire any action from the Live screen and watch it
  appear in the activity feed, without touching config.
- **Fn layers** — function-key layers for pad keyboards.
- **Export / import** — base64 config backup and restore straight from the
  dashboard.
- **Typed Action Builder** — visual multi-app action builder with an insertable
  action grammar.

## Install

1. Extract the downloaded `KeySidekick-*.zip` archive to any folder (e.g. `C:\KeySidekick`).
2. Run `run.bat` — on first start it creates `config.ini` from
   `config.example.ini` and launches `sidekick.exe`.
3. Open the dashboard: `http://127.0.0.1:8765/`.
4. Click **+ Setup keyboard** and follow the wizard — the one-time driver swap
   uses [Zadig](https://zadig.akeo.ie/).

## ⚠ One-time Zadig warning

After the driver swap your keyboard **stops typing on its own** — its keys no
longer reach the foreground window and are read by KeySidekick instead. This is
intended and is how the tool intercepts input. Keep a second keyboard (or the
on-screen keyboard) handy while you configure profiles.

## Rollback

The driver swap is reversible. See
[`ZADIG_INSTRUCTIONS.md`](ZADIG_INSTRUCTIONS.md) for the step-by-step undo
procedure (reinstall the native HID keyboard driver via Zadig).

## Known limitations

Read the **Known limitations (read before use)** section of
[`README.md`](README.md) before relying on this in games or anti-cheat
environments — basic-mode re-injection uses `SendInput` and is detected by
anti-cheat software.
