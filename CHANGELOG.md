# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed

- **"Window not found" now says why** — the test-connection check in the dashboard reports how many top-level windows expose no process information (protected or sandboxed apps), because matching by process/path is impossible for them; the same count is logged when a targeted key cannot resolve its window.

### Changed

- **The MSVC analyzer gate cannot silently disappear** — `src/build.bat --check-msvc` now fails when it runs under CI (where the toolset is guaranteed) and no Visual Studio is found, instead of skipping; a local run without VS still skips with exit 0. Its temporary files are per-run, so two gate runs no longer collide.

## [0.9.8] - 2026-09-20

Follow-up to the 0.9.7 audit: removes the last dead module, closes two defects
that were left open, and makes the checks themselves honest.

### Fixed

- **Deleting a profile that other profiles switch to is refused now** — the guard existed but could never fire: actions are stored as raw `!switch:` / `!toggle:` strings, which the check did not look at, so a profile could be deleted while mappings still pointed at it. The check now matches both the profile id and its display name, case-insensitively, and understands the carrier string.
- **A key can no longer be delivered to the wrong window** — in targeted mode the ledger remembers the target's process id and window class and re-validates them before every repeat and key-up. Previously only `IsWindow()` was checked, so a window that inherited a recycled handle value received repeats and a key-up it never saw a key-down for.
- **`probe_device.exe` reports the truth** — the interface line is printed again (a failed settings query was swallowed and the endpoint loop then iterated a garbage count), the HID report length is read only when the full 9-byte descriptor arrived, a too-long device path is reported instead of handing an uninitialised buffer to `CreateFileW`, and the exit code reflects probe failures (2 = every matched device failed) instead of always reporting success.
- **Dashboard feedback** — a failed request now always surfaces as a message: one global handler covers every action that had no error handling; the live-update revision comparison works (it compared a string with a number, so the deduplication never triggered); Help no longer leaves background polls running; returning the keyboard to the standard driver asks for confirmation first.
- **Dead variable removed** (`g_trayIcon`, unused since 0.9.x — found by the stricter warning gate below).

### Changed

- **Removed the unused Task Scheduler startup module** (`src/startup_manager.*` and its test suite, never linked into `sidekick.exe`). Autostart in the shipped build is the Startup-folder shortcut, verified end-to-end: enabling creates the shortcut, `GET /api/v1/startup` reports it, disabling removes it. Test suites: 15.

### Added

- **`src/build.bat --check-msvc`** — a second gate using MSVC's `cl /analyze` over the same translation units (reviewed codes suppressed explicitly), now part of CI alongside the GCC warning gate.
- **The warning gate compiles for real** — it used `-fsyntax-only`, which never reaches the pass that reports unused file-scope variables; that is exactly how the dead `g_trayIcon` survived to a release. It now compiles to an object file, and a deliberate unused variable makes it fail (verified).
- **Release notes are kept in sync with the published release** — a workflow updates the body of the published release from `RELEASE-NOTES-template.md` when the file changes, because the release action only refreshes assets on re-runs.


## [0.9.7] - 2026-09-19

Patch release after a full audit of the repository (68 findings). Nothing new in
the UI — fixes, tests and regression gates only.

### Fixed

- **The app died after a few minutes of dashboard polling** — the HTTP handler never closed the accepted socket on the normal path, so every request (the dashboard polls state continuously) leaked a descriptor: measured +300 descriptors per 300 requests, after which `accept` failed while the process kept running.
- **An action could fire twice** — an action-mapped key was dropped from the tracked held-set, so pressing any other key while it was held re-fired the macro / app launch / profile switch.
- **Stuck modifiers** — the modifier diff was skipped whenever the same report contained an action key, so a released Ctrl/Shift/Alt/Win stayed held system-wide. The identify window now also releases injected keys when it opens.
- **`;`, Right Arrow and Pause did not work in basic mode** — the `;` row of the scan-code table was commented out by a trailing backslash in a comment, Right was mapped to End's scan code and Pause to Insert's.
- **`sidekick.exe --driver swap|restore|status` never found the device** — the id was built from every hex character of the argument (including the `d` in `vid_`/`pid_`) and compared in the wrong case, so the tool answered "is the keyboard plugged in?" for a present device; the dashboard's driver buttons and the post-port-change recovery were dead for the same reason. `--driver status` now also prints which interface swap/restore would target, and refuses to guess when several interfaces match (it used to be able to bind WinUSB to the composite parent node).
- **Crash after resume from sleep** when the keyboard could not be reopened: `WinUsb_ReadPipe` was called with a closed handle instead of waiting for the device.
- **`GET /api/v1/devices` returned a truncated VID/PID** (`vid_0406&`), so the device list did not match `/api/v1/hid`.
- **A profile with `AutoStart=1` lost the setting** — the flag was not carried between the file and the internal model, so the "launch the app if the window is missing" checkbox reset on the first save.
- **Unknown `config.ini` lines were dropped** on the first save from the dashboard even though the parser promised to keep them (they are now preserved as `[Extension.*]`).
- **A failed `config.ini` write was reported as success** — the edit stayed in memory and vanished on restart; the operation now reports the failure (verified on a read-only file: HTTP 500 instead of `{"ok":true}`).
- **The configured port could diverge from the live listener** — after importing a config with a different `HTTPPort` every request got 403 while the server was up; the security policy and URLs now use the port the listener actually bound, and the file value applies on the next start.
- **A garbage config was silently treated as empty** and overwritten; a file without a single recognized section is now reported as an error in the log.
- **DOM XSS in the dashboard** — an action string was injected into an inline handler without escaping the backslash, so a crafted string (including one from an imported foreign config) executed arbitrary JS in the dashboard origin where the CSRF token lives. The existing `jsStr()` is used now.
- **Race on the injection ledgers** — HTTP workers released keys while the read loop was mutating the same containers.
- **`POST /api/profile/activate` answered success** even when the profile did not exist; `POST /api/reload` answered success even when the config was unreadable.
- **A race in the threading shim** made the concurrency tests meaningless: `join()` returned before the threads ran (it copied an owning HANDLE).

### Added

- **Compiler warning gate**: `src/build.bat --check-warnings` (12 translation units, fails on any warning or error), the same flags in the normal build and in the test runner, and a dedicated CI step.
- **New test suites**: `report_diff` (HID report edge detection) and `mingw_threading` (shim threads/mutexes/condition variable); 16 suites and 58 HTTP checks in total.

### Security

- Dashboard: escaping in diagnostics and inline handlers; a single list of "mutating" paths, with `/api/v1/devices/detect` moved to POST with the CSRF token required (a GET without a token used to release held keys).
- The release ZIP no longer ships the developer's private `src/config.ini`.

## [0.9.6] - 2026-08-13

### Fixed

- **Identify feed pollution by own re-injection** — while the dashboard's keypress-identify window is active (15s), basic mode stops re-injecting the dedicated keyboard: its keys no longer type into the wizard itself and no longer appear in the Raw Input feed as "can't attribute" (those were KeySidekick's own SendInput events, which arrive with a NULL device handle).
- **Modifier safety release on startup** — a hard-killed previous instance could leave injected modifiers pressed forever (SendInput survives the injector process); KeySidekick now releases Shift/Ctrl/Alt/Win on start.

## [0.9.5] - 2026-08-13

### Fixed

- **Stuck injected modifiers (auto Ctrl) during wizard/device tests** — the detect/capture probes opened the SAME WinUSB interface the read loop was reading, stealing interrupt-IN reports: a key-up was lost and the injected Ctrl/Shift stayed down until the keyboard was unplugged. Probes now skip the active interface (`IsActiveDevicePath`), and both probe endpoints release all injected keys defensively before probing.

## [0.9.4] - 2026-08-12

### Changed

- **Driver helper merged into sidekick.exe** — no separate `ks_driver.exe`: `sidekick.exe --driver swap|restore|status vid_xxxx&pid_yyyy` is the CLI mode. The app never runs elevated; the UAC prompt appears only at the swap/restore moment (self-elevation inside the driver command).
- **Portable & offline documented** — README (EN/RU/ZH): carry the folder on a USB stick, profiles travel in `config.ini`; the program makes zero network connections (the only socket is the loopback dashboard) — no telemetry, no cloud, verified by code audit.
- Wizard shows "Profiles ready (N) — your portable setup is fully restored" after activation.

## [0.9.3] - 2026-08-12

### Added

- **Native driver swap without Zadig** — bundled `ks_driver.exe` binds the Microsoft-signed inbox `winusb.inf` to the keyboard (Windows 10 1809+, no self-signed certificates, no downloads) and restores the inbox HID driver (`input.inf`) on demand. Portable: rollback works even after KeySidekick is removed.
- **Port-change detection** — when the keyboard is plugged into a different USB port, Windows binds the new node to the default HID driver; KeySidekick now detects that the configured VID/PID reappeared as an ordinary keyboard, flags it in the dashboard/API (`portChangeDetected`), and offers a one-click "Apply driver again" — profiles are kept per VID/PID and come back automatically.
- **Dashboard driver controls** — devices page: port-change banner with one-click re-apply, "Restore original driver" per ready device; wizard step 3: "Swap automatically (UAC prompt)" as the primary path with the Zadig manual steps collapsed as fallback.
- `POST /api/v1/driver/swap` and `POST /api/v1/driver/restore` HTTP endpoints (CSRF-protected, vidpid format-validated, elevation gated by UAC consent).

### Changed

- Release ZIP includes `ks_driver.exe`; integration suite covers the new endpoints (58 checks).

## [0.9.2] - 2026-08-12

### Security

- **Dashboard XSS closed** — window-picker inline handlers escaped `&` and `"` (crafted window class names could run script in the dashboard origin and steal the CSRF token).
- **SSE CORS** — `Access-Control-Allow-Origin` is echoed only for loopback origins, never `*`; evil-origin requests are rejected by the security pipeline with 403.
- **CSRF coverage widened** — every POST-only route (v1 CRUD, presets, activate, fire, capture) returns 405 on GET/HEAD via a declarative `IsPostOnlyPath` list.
- **HTTP worker pool (slowloris)** — connection handling moved to a bounded pool of 8 workers; a slow client no longer stalls the whole dashboard; pool overflow rejects instantly. `WriteConfig`, device-info reads and the state revision are synchronized for concurrent handlers.
- **Clean shutdown** — SSE clients and HTTP workers are drained, the accept thread is joined, and critical sections are deleted only afterwards; the console handler no longer races WinUSB teardown (abort happens on the owning thread via `WM_USER_SHUTDOWN`).
- **JSON hardening** — `JsonGetStr` fully decodes escapes (`\uXXXX`, surrogate pairs); `JsonEscape` escapes `<`, `>`, `&`; user-controlled strings are never truncated by fixed buffers in `/api/status`, `/api/v1/state`, `/foreground`, `test-resolve`.
- **Predictable temp file fixed** — config writes use an unpredictable temp name plus `CREATE_NEW` retry, closing the pre-created-junction race.
- **Targeted dashboard-open message** — second-instance "open dashboard" uses `FindWindow` instead of `HWND_BROADCAST` (and the receiver was missing entirely — wired now).

### Fixed

- **Auto-start toggle was inverted** — the dashboard sent a JSON boolean, the server parsed strings: enabling auto-start deleted the shortcut while reporting success. `JsonGetBool`/`JsonGetInt` now accept booleans, and the UI surfaces the real `installed` state.
- **`GET /api/v1/windows/foreground` was missing `processPath`** — the launch picker feature was dead; the field is included now.
- **DashOp buffers** — all `strncpy` fills of fixed buffers replaced with terminating `snprintf`; `MultiByteToWideChar` results checked in launch/window lookup.
- **Truncated request bodies** — a clean close with an incomplete body is no longer dispatched (partial config import could corrupt `config.ini`).
- **SSE hot-path** — pushes use non-blocking sockets and drop slow clients instead of stalling the device loop (up to 1.6s per keystroke).
- **Startup-manager test suite was orphaned** — wired into the runner (14 suites).
- **Idle cleanliness (event-driven)** — with the keyboard disconnected the app no longer polls SetupAPI every 500ms or writes logs periodically: it sleeps in `MsgWaitForMultipleObjectsEx` and wakes on `WM_DEVICECHANGE` with exponential backoff (2s→60s) as a safety net. Auto-switch caches the foreground HWND (no per-second process queries). Measured idle: 0 disk I/O, ~0% CPU.
- **Device-loss path** — a failed reconnect now returns to the event-driven wait instead of spinning in the read loop.
- **Onboarding honesty** — "Control an app" and "Custom mix" now really create the profile + application target and activate it; the final screen checks the real auto-start state instead of claiming it; all wizard/identify/capture polls stop when leaving their screens.

### Changed

- **Trilingual presentation** (`presentation.html`) with a language switcher; English preset descriptions.
- **Docs sync** — CHANGELOG 0.9.1 claim corrected (legacy GET `/switch` returns 405), FAQ points at `/api/v1/state`, READMEs describe the event-driven idle model and the auto-restoring integration suite.
- **CI** — release metadata validation (tag == `APP_VERSION`, release notes must mention the version); the vacuous dashboard-staleness step was removed; release ZIP now ships Corresponding Source (GPL-3.0 §6).

## [0.9.1] - 2026-08-11

### Changed

- **Security hardening** — CSRF token (`X-KeySidekick-Token`) enforced on all mutating routes: every `POST` is rejected with 403 without a valid token, plus Origin allow-listing; profile switching is POST-only via `POST /api/profile/activate` (legacy GET `/switch` and GET `/profile` now return 405; the only switch path is `POST /api/profile/activate`).
- **WinUSB teardown fix** — pending interrupt reads are aborted (`WinUsb_AbortPipe`) before `WinUsb_Free`/`CloseHandle` on shutdown and power-resume paths, so the device handle is released cleanly.
- **HTTP/SSE robustness** — the SSE stream (`/api/v1/events`) keeps connections alive, and the device loop no longer exits when the keyboard is absent — it retries with bounded backoff while the dashboard/HTTP keep working.
- **Hardware-binding cleanup** — no author hardware in defaults or examples: the `DeviceVIDPID` default is now empty (read from config) and `config.example.ini`/README ship the `vid_xxxx&pid_yyyy` placeholder.
- **Onboarding wizard polish** — prep-checklist gate before the driver-swap step (typing test, auto-start, and fallback input must be confirmed; persisted in `ks_prep_state`), "no keyboard active" note on first-run onboarding, updated identify flow copy.
- **Mouse-button identification in the key-press step** — the identify feed now labels mouse buttons (LMB / RMB / MMB / back / forward) and the setup UI guides "press a key or click a mouse button" to identify a device.

## [0.9.0] - 2026-08-11

### Added

- **Driver-swap onboarding wizard** — HID-first device list, keypress identification, and step-by-step Zadig guidance (including the phantom-copy warning and rollback instructions).
- **AI-agent pad presets and use-case templates** — media, OBS, meetings, PowerPoint/office, REAPER, DaVinci Resolve, Ableton Live, Adobe Premiere, and Lightroom Classic.
- **Combo macros** (`{Ctrl+B}`, …) and multi-key sequences as actions.
- **Live screen** with click-to-fire actions (no physical key needed).
- **Fn layers** — modifier layer support per profile.
- **Config export/import** from the dashboard.
- **Typed Action Builder** — modal action editor replacing browser `prompt()`.
- **First-run onboarding** on an empty dashboard (Pad template / Create profile / Keyboard setup).
- **SSE live updates** — event-driven dashboard refresh instead of polling.
- **Per-instance device activation** by full device path (e.g. two identical keyboards).
- **Autostart manager** — start KeySidekick with Windows from the dashboard or API.
- **Orphan cleanup on profile delete** — unreferenced preset-created applications are removed automatically.
