# The problem and the solution

A deep-dive into why dedicating a keyboard to one app on Windows is harder than it looks, and how WinUSB solves it. Start here if you want to understand *why* the design is what it is.

## The goal

Route keystrokes from one specific physical USB keyboard to a chosen application, and **swallow** them so they never reach the foreground window. The user's main keyboard must keep working normally, with no lag. It should survive hibernate/sleep. Ideally no kernel-mode driver.

## The Windows input stack (why this is hard)

When you press a key on any keyboard, it travels through a fixed pipeline:

```
USB keyboard
   → hidclass.sys        (HID class driver, parses the HID report)
   → kbdhid.sys          (HID keyboard mapper: translates HID usages → scan codes)
   → kbdclass.sys        (keyboard class driver: system-wide input queue)
   → RIT (Raw Input Thread, in csrss.exe)
   → foreground window's thread → WM_KEYDOWN / WM_CHAR / etc.
```

The critical fact: **`kbdclass.sys` merges all keyboards into a single logical stream.** By the time input reaches the foreground window, there's no per-device identity left. Three consequences:

1. A `WH_KEYBOARD_LL` hook can block keys, but the `KBDLLHOOKSTRUCT` it sees has **no device identifier** (`vkCode`, `scanCode`, `flags`, `time`, `dwExtraInfo` — that's it). You cannot tell which keyboard sent a key.

2. `Raw Input` (`RegisterRawInputDevices` + `WM_INPUT`) *does* give you the device handle (`RAWINPUTHEADER.hDevice`) — but it's **read-only**. It cannot swallow a key. Blocking must happen elsewhere.

3. Worse: for many real keyboards, `hDevice` is **NULL** in `WM_INPUT`.

### Why `hDevice == NULL` for some keyboards

This is the crux. Microsoft documents it ([Microsoft Q&A](https://learn.microsoft.com/en-us/answers/questions/1666513/getrawinputdata-header-hdevice-is-null-for-builtin), [Stack Overflow](https://stackoverflow.com/questions/57552844/rawinputheader-hdevice-null-on-wm-input-for-laptop-trackpad)):

> *External devices report a non-NULL `hDevice`, but builtin/transposed devices may report NULL — this is a known OS limitation.*

In practice, NULL `hDevice` shows up for:
- Laptops with a precision touchpad
- **Composite USB devices** that expose a keyboard alongside other interfaces (touchpad, consumer control, system control) — exactly the cheap "keyboard with integrated trackpad" form factor
- Some HID converters and virtual bridges

When `hDevice` is NULL, Raw Input cannot distinguish that keyboard from the main keyboard — both look anonymous. The "identify by Raw Input, block by LL hook" hybrid then produces false positives (main-keyboard keys routed to the target app) or false negatives.

You can verify this empirically: register Raw Input, log `hDevice` for each `WM_INPUT`. A normal external USB keyboard returns a stable non-NULL handle. A keyboard+touchpad composite returns NULL for *all* its events, keyboard included.

## Why the obvious hybrids fail

### Raw Input + `WH_KEYBOARD_LL` (the "obvious" approach)

Identify the device in Raw Input, then have the LL hook block the matching key. Two fatal flaws:

1. **The LL hook runs on the system input thread.** Any code in the hook that waits (e.g. to correlate with the Raw-Input thread, which runs on a different thread and may not have recorded the device yet) delays *all* keyboard input system-wide. If the hook stalls past `LowLevelHooksTimeout` (default 300 ms), Windows silently disables it. In practice, even a ~40 ms correlation wait makes the system unusably sluggish.

2. **For NULL-`hDevice` devices, the correlation can't work** — see above.

### Interception / AutoHotInterception (the "just install a driver" approach)

A keyboard class upper-filter driver (`UpperFilters = keyboard, kbdclass`) intercepts at the device stack level, so it can both identify and block per-device. It works. But:

- It's a kernel driver (unsigned in the free Interception variant).
- On hibernate/resume, the PnP manager re-enumerates USB and can exhaust Interception's fixed hardware-slot count, causing a **system-wide input lockup** requiring a hard reboot ([Interception issue #181](https://github.com/oblitum/Interception/issues/181)).
- AutoHotInterception sometimes returns corrupted VID/PID (`0x0000`) for some devices, breaking device lookup.

## The solution: WinUSB Driver-Replacement Bypass

Replace the function driver bound to the keyboard's `MI_00` interface. Swap `hidusb.sys` (which feeds `kbdhid.sys → kbdclass.sys`, i.e. the system keyboard stack) for Microsoft's **`WinUSB.sys`** — a generic, WHQL-signed, user-mode-readable USB driver.

Two things happen:

1. **The keyboard stops being a system keyboard.** With no `kbdhid.sys` bound, its keys are never translated to scan codes, never enter `kbdclass.sys`, never reach the foreground window. There's nothing to block — the keys simply don't go anywhere by default. No LL hook needed, no system input thread touched, zero lag.

2. **The device becomes directly readable in user mode.** `WinUSB.sys` exposes the device via the WinUSB API; a user-mode program opens it with `CreateFileW` and reads the raw 8-byte HID keyboard reports from the interrupt IN endpoint with an overlapped `WinUsb_ReadPipe` loop.

```
[Dedicated keyboard MI_00]
   → [WinUSB.sys]   (replaced hidusb.sys via Zadig)
   → [user-mode sidekick.exe]
        ├─ SetupAPI: find device by GUID + VID/PID
        ├─ CreateFileW(... GENERIC_READ|WRITE, FILE_FLAG_OVERLAPPED ...)
        ├─ WinUsb_Initialize
        ├─ overlapped WinUsb_ReadPipe on interrupt IN endpoint
        ├─ parse 8-byte HID keyboard report
        ├─ edge-detect key-down (Usage IDs in bytes 2–7)
        ├─ map Usage ID → action per the active profile
        └─ dispatch: PostMessage to target window / SendInput re-inject / switch profile
[Main keyboard] → standard hidusb/kbdhid/kbdclass stack → works normally
```

The main keyboard is entirely untouched. The dedicated keyboard can't leak into the foreground because it has no path *to* the foreground.

### Why this is strictly better

| | WinUSB bypass | Raw Input + LL hook | Interception |
|---|---|---|---|
| Distinguishes the keyboard | ✅ (reads it directly) | ⚠️ fails if `hDevice==NULL` | ✅ |
| Blocks keys from foreground | ✅ (no path exists) | ✅ via hook | ✅ |
| System lag | ✅ none | ❌ hook blocks input thread | ✅ none |
| Works for composite/cheap keyboards | ✅ | ❌ NULL `hDevice` | ✅ |
| Kernel driver | ❌ no (WinUSB is built-in, WHQL) | ❌ no | ✅ yes (unsigned) |
| Hibernate-safe | ✅ (WinUSB handles ACPI) | ✅ | ❌ slot-exhaustion lockup |
| One-time setup | Zadig swap | none | driver install + reboot |

## Implementation notes (the non-obvious traps)

These are the things that cost real debugging time. Documented so you don't repeat them.

### WinUSB requires `FILE_FLAG_OVERLAPPED` on the handle

Without it, `WinUsb_Initialize` returns `ERROR_INVALID_HANDLE` (6). This is not optional and not obvious from the docs. The router opens the device with `FILE_FLAG_OVERLAPPED` and uses an overlapped read loop.

### Don't `WinUsb_AbortPipe` on a pending read just because a poll timed out

That leaks/corrupts the overlapped state. Leave the overlapped read pending and `WaitForSingleObject` on its event again next iteration — the interrupt endpoint signals the event when data arrives. Only call `AbortPipe` once, at shutdown, to release the pending read before closing. (See the router's `ReadLoop`.)

### Set sensible pipe policies

```c
UCHAR rawIo = 0;  WinUsb_SetPipePolicy(h, pipe, RAW_IO, sizeof(rawIo), &rawIo);              // 0 = full HID reports (not raw)
ULONG timeoutMs = 0; WinUsb_SetPipePolicy(h, pipe, PIPE_TRANSFER_TIMEOUT, sizeof(timeoutMs), &timeoutMs); // 0 = wait forever (overlapped)
```

### The standard HID keyboard report is 8 bytes

```
byte 0: modifier bitmask (LCtrl=0x01, LShift=0x02, LAlt=0x04, LGUI=0x08, RCtrl=0x10, RShift=0x20, RAlt=0x40, RGUI=0x80)
byte 1: reserved (0)
bytes 2–7: up to 6 simultaneously-held HID Usage IDs (0 = empty slot)
```
Confirmed by reading the device's HID report descriptor. Reports are *state* (the currently-held set), not events — you diff against the previous report to extract key-down/key-up edges.

### HID Usage ID ≠ Set-1 scan code (the SendInput trap)

For basic mode (re-inject via `SendInput`), `KEYBDINPUT.wScan` expects **Set-1 make codes**, not HID Usage IDs. They're different numbering schemes:

| Key | HID Usage ID | Set-1 scan |
|---|---|---|
| A | `0x04` | `0x1E` |
| 1 | `0x1E` | `0x02` |

If you naively put Usage `0x04` into `wScan`, you get the `3` key, not `a`. The router has a built-in `UsageToSet1()` translation table (built from [Microsoft's USB HID to PS/2 translation table](https://download.microsoft.com/download/1/6/1/161ba512-40e2-4cc9-843a-923143f3456c/translate.pdf)). Extended keys (arrows `0x4F-0x52`, nav cluster `0x49-0x4E`, Numpad Enter, right-side modifiers) need `KEYEVENTF_EXTENDEDKEY`. Modifiers are independent key events (diff the modifier byte, send KEYDOWN/KEYUP per changed bit), not a per-keystroke field.

### `SendInput` sets `LLKHF_INJECTED`

Every event the router re-injects in basic mode carries this flag. Normal apps (browsers, editors, terminals) ignore it. Games and anti-cheat inspect it and ignore or flag the input. This is inherent to `SendInput` and unavoidable in user mode. Document it to users — basic mode is for productivity apps, not games.

### Zadig's phantom-copy gotcha

A device that's been replugged many times accumulates dozens of `CM_PROB_PHANTOM` registry entries. Zadig may replace one of those instead of the live instance, leaving the active driver untouched. Always verify the swap took effect with `Get-PnpDevice -Present` and check `DEVPKEY_Device_Service == WinUSB`. If not, use Zadig's **Reinstall Driver** (not Replace). See [ZADIG_INSTRUCTIONS.md](../ZADIG_INSTRUCTIONS.md).

### Power management

WinUSB handles USB ACPI transitions natively; the router just reopens its handle on resume. Register for `WM_POWERBROADCAST` (`PBT_APMRESUMESUSPEND`/`CRITICAL`/`AUTOMATIC` → close handle, re-enumerate via SetupAPI, reopen). No driver re-install, no slot exhaustion, no system lockup. This is the whole reason WinUSB is preferred over Interception for laptops / sleep-heavy setups.

## Sources

- [Keyboard and mouse HID client drivers — Microsoft Docs](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/keyboard-and-mouse-hid-client-drivers)
- [About keyboard input — Microsoft Docs](https://learn.microsoft.com/en-us/windows/win32/inputdev/about-keyboard-input)
- [RegisterRawInputDevices — Microsoft Docs](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registerrawinputdevices)
- [`hDevice` NULL for builtin/touchpad devices — Microsoft Q&A](https://learn.microsoft.com/en-us/answers/questions/1666513/getrawinputdata-header-hdevice-is-null-for-builtin)
- [RAWINPUTHEADER hDevice null on WM_INPUT for laptop trackpad — Stack Overflow](https://stackoverflow.com/questions/57552844/rawinputheader-hdevice-null-on-wm-input-for-laptop-trackpad)
- [KEYBDINPUT (winuser.h) — Microsoft Docs](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-keybdinput)
- [USB HID to PS/2 Scan Code Translation Table — Microsoft](https://download.microsoft.com/download/1/6/1/161ba512-40e2-4cc9-843a-923143f3456c/translate.pdf)
- [USB HID Usage Tables — USB-IF](https://usb.org/sites/default/files/hut1_22.pdf)
- [Access a USB device by using WinUSB functions — Microsoft Docs](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/using-winusb-api-to-communicate-with-a-usb-device)
- [Interception hibernate lockup — issue #181](https://github.com/oblitum/Interception/issues/181)
- [Zadig — Pete Batard / Akeo](https://zadig.akeo.ie/)
