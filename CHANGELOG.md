# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.9.1] - 2026-08-11

### Changed

- **Security hardening** — CSRF token (`X-KeySidekick-Token`) enforced on all mutating routes: every `POST` is rejected with 403 without a valid token, plus Origin allow-listing; profile switching is POST-only via `POST /api/profile/activate` (legacy GET `/switch` kept only as undocumented backward-compat).
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
