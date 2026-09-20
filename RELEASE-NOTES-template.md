# KeySidekick 0.9.8 — Release Notes

Follow-up to the 0.9.7 audit. Nothing new in the UI — the last dead module is
gone, two defects that were left open are fixed, and the checks themselves are
now honest.

## Fixed — what you actually notice

- **You can no longer delete a profile that other profiles switch to.** The guard
  existed but never fired (actions are stored as raw `!switch:` / `!toggle:`
  strings, which the check did not inspect), so deleting such a profile left
  mappings pointing at nothing. It now refuses and names the profile.
- **A key in targeted mode cannot land in the wrong window anymore.** The ledger
  remembers the target's process and window class and re-checks them before every
  repeat and key-up. Before, only "is this still a window?" was checked, so after
  the target closed, a window that inherited the same handle value could receive
  repeats — and a key-up it never saw a key-down for.
- **The diagnostic tool `probe_device.exe` tells the truth**: it prints the
  interface line again (a failed query used to be swallowed and the endpoint loop
  then ran over a garbage count), reads the HID report length only from a
  complete 9-byte descriptor, reports an over-long device path instead of passing
  an uninitialised buffer to Windows, and returns a non-zero code when probing
  failed instead of always reporting success.
- **Dashboard feedback and safety**: a failed request always shows a message now
  (previously about twenty actions failed silently), live updates compare the
  state revision correctly (a string was being compared with a number, so the
  deduplication never worked), the Help screen no longer leaves background polls
  running, and returning the keyboard to the standard driver asks for
  confirmation before doing it.
- **Dead code removed**: an unused tray-icon variable that had been in the source
  since 0.9.x.

## Changed

- **The unused Task Scheduler startup module is gone** (`src/startup_manager.*`
  and its test suite — it was never linked into `sidekick.exe`). Autostart in the
  shipped build is the Startup-folder shortcut, verified end-to-end: enabling
  creates the shortcut, `GET /api/v1/startup` reports it, disabling removes it.
  Test suites: 15.

## Added

- **A second static-analysis gate** — `src/build.bat --check-msvc` runs MSVC's
  `cl /analyze` over the same translation units (reviewed and accepted codes are
  suppressed explicitly) and is part of CI next to the GCC warning gate.
- **The warning gate now compiles for real.** It used to run with
  `-fsyntax-only`, which never reaches the pass that reports unused file-scope
  variables — exactly how the dead variable above survived into a release. A
  deliberate unused variable now makes the gate fail (verified).
- **Release notes are kept in sync with the published release page** — a workflow
  updates the body of the published release from `RELEASE-NOTES-template.md` when
  that file changes, because the release action only refreshes assets on re-runs.

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
