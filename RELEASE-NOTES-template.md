# KeySidekick 0.9.7 — Release Notes

Patch release after a full audit of the repository (68 findings). Nothing new in
the UI — fixes, tests and regression gates only.

## Fixed — what you actually notice

- **The app no longer dies after a few minutes of use** — the HTTP server leaked a
  socket per request, and the dashboard polls state continuously (measured: +300
  descriptors per 300 requests). It looked like "the dashboard stopped
  responding" while the process was still running.
- **An action fires exactly once** — holding an action key and pressing another
  one used to re-fire the macro, relaunch the app or switch the profile again.
- **Keys no longer stick** — a released Ctrl/Shift/Alt/Win stayed held
  system-wide; the keyboard-identify window now also releases previously injected
  keys when it opens.
- **`;`, Right Arrow and Pause work again** in basic mode: the `;` row of the
  scan-code table had been commented out in the source, Right typed End and Pause
  typed Insert.
- **Driver switching works**: `sidekick.exe --driver swap|restore|status` never
  found the device and reported "is the keyboard plugged in?"; the dashboard's
  driver buttons and the recovery after moving the keyboard to another USB port
  were dead for the same reason. `--driver status` now shows which interface
  swap/restore would target, and refuses to guess when several interfaces match
  (it used to be able to bind WinUSB to the composite parent node, which would
  stop the keyboard from typing).
- **No crash after resume from sleep** when the keyboard cannot be reopened.
- **Settings are kept**: profile `AutoStart`, unknown `config.ini` lines, and an
  honest error when the config cannot be written (the dashboard used to say
  "saved" while the edit vanished on restart).
- **Importing a config with a different port** no longer turns the dashboard into
  "403 on everything": the security policy uses the port the server actually
  bound, and the file value applies on the next start.
- **A garbage or foreign `config.ini`** is reported as an error in the log
  instead of being silently replaced with defaults.
- **Dashboard XSS closed** — a crafted action string (including one coming from an
  imported foreign config) executed arbitrary code in the dashboard origin, where
  the CSRF token lives.
- **The device list** (`/api/v1/devices`) returns the full VID/PID instead of a
  truncated one, so it matches the data from `/api/v1/hid`.

## Added

- **Compiler warning gate** — `src/build.bat --check-warnings` checks all 12
  translation units and fails on any warning or error; the same flags are enabled
  in the normal build and in the test suite, with a dedicated CI step. The
  absence of such a check is exactly what let three defects (including the
  non-working `;` key) reach a release.
- **Tests:** 16 suites (new: `report_diff` for HID report edge detection, and
  `mingw_threading` for the threading shim) and 58 HTTP checks against a live
  server.

## Install

1. Extract the downloaded `KeySidekick-*.zip` archive to any folder (e.g. `C:\KeySidekick`).
2. Run `run.bat` — on first start it creates `config.ini` from
   `config.example.ini` and launches `sidekick.exe`.
3. Open the dashboard: `http://127.0.0.1:8765/`.
4. Click **+ Setup keyboard** and follow the wizard. The driver swap is built in
   (`sidekick.exe --driver swap`, UAC prompt once); [Zadig](https://zadig.akeo.ie/)
   remains a manual fallback.

## ⚠ One-time driver warning

After the driver swap the dedicated keyboard **stops typing on its own** — its
keys are read by KeySidekick and sent to the configured profiles instead. This is
intended: keep a second keyboard (or the on-screen keyboard) handy while you set
up profiles.

## Rollback

The driver swap is reversible: `sidekick.exe --driver restore vid_xxxx&pid_yyyy`
(or `--driver status` to see the current state of the device nodes). The manual
[ZADIG_INSTRUCTIONS.md](ZADIG_INSTRUCTIONS.md) path remains as a fallback.

## Known limitations

Read the **Known limitations (read before use)** section of
[`README.md`](README.md) before relying on this in games or anti-cheat
environments — basic-mode re-injection uses `SendInput` and is detected by
anti-cheat software.
