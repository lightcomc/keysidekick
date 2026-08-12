# FAQ

## Setup & driver

### After Zadig, my keyboard stopped typing entirely

That's expected and correct. Once the `MI_00` interface is on the WinUSB driver, the keyboard is no longer a system keyboard — Windows has nothing to route its keys to. The `sidekick.exe` program is what reads those keys and decides where they go (basic mode re-injects them so it types normally; targeted mode sends them to your chosen app). Start `sidekick.exe` and switch to the `basic` profile to confirm it types again.

### Zadig said "SUCCESS" but the keyboard still types like normal

Zadig sometimes replaces a *phantom* (disconnected) copy of the device instead of the live one — common when the keyboard has been unplugged/replugged many times (you'll see many entries with the same VID/PID in Device Manager).

Fix: in Zadig, select the device and click **Reinstall Driver** (not Replace). Then verify the swap actually took:

```powershell
Get-PnpDevice -Present | Where-Object { $_.InstanceId -like '*VID_xxxx&PID_yyyy*MI_00*' } |
    Get-PnpDeviceProperty -KeyName 'DEVPKEY_Device_Service' |
    Select-Object InstanceId, Data
```

Expected: `Data : WinUSB`. If it still says `HidUsb`, the swap didn't hit the active instance.

See [ZADIG_INSTRUCTIONS.md](../ZADIG_INSTRUCTIONS.md#common-problem-phantom-copy-got-replaced-instead).

### I broke my touchpad / mouse after Zadig

You picked the wrong interface — `MI_01` (touchpad/mouse/consumer) instead of `MI_00` (keyboard). Rollback:

1. Device Manager → find the device (now under "Universal Serial Bus devices") → right-click → **Uninstall device** ✓ "Delete the driver software".
2. Action menu → **Scan for hardware changes**. Windows reinstalls the default driver.

To avoid this: before Zadig, check which `MI_xx` is the keyboard:
```powershell
Get-PnpDevice -Present | Where-Object { $_.InstanceId -like '*VID_xxxx&PID_yyyy*' } |
    Select-Object FriendlyName, InstanceId, Class | Format-Table -AutoSize -Wrap
```
The one with `Class = Keyboard` is your `MI_00`. Touchpad/mouse will show `Class = Mouse`.

## Behavior

### The target app reacts to my MAIN keyboard too

That's the app's own global/local hotkeys, not this tool. Many apps (AIMP, VLC, media players) bind keys like `1`, `Q`, `Space` in their own settings. When you press them on the main keyboard while that app has focus (or has registered them globally), the app reacts on its own — the router isn't involved and can't prevent it.

To verify it's not the router: kill `sidekick.exe`. If the main keyboard still triggers the app, it's the app's own hotkeys. Either remove them in the app's settings, or keep the app out of focus when typing.

### My target-app keys also type into the foreground window

That shouldn't happen in `targeted` mode — the keys go to the target window via `PostMessage` and never reach the foreground. If you see double input:
- You're probably in `basic` profile (check the tray tooltip or `curl http://127.0.0.1:8765/api/v1/state`, which returns the active profile name). Switch to the targeted profile.
- Or the key has no mapping in the active profile, so in basic mode it falls through to re-injection. Add a mapping or stay on the targeted profile.

### Does holding a key work in targeted mode?

Yes for a single-key mapping such as `w`, `{F1}`, or `{Volume_Up}`. KeySidekick sends the initial `WM_KEYDOWN`, repeats it using the Windows keyboard delay/speed settings, and sends `WM_KEYUP` when the physical key is released. Profile switching, config reload, disconnect, suspend, and clean exit release every owned targeted key.

Dashboard labels like `E / У` or `M / Ь` identify one physical key in two layouts. In the Action choose one Latin virtual key (`e`, `m`, `r`, and so on); do not copy the combined `E / У` label into the Action field.

Commands such as `!switch`, `!toggle`, and `!launch`, plus multi-key sequences and `!app` actions, remain one-shot/tap actions. Also note that `PostMessage` does not change global keyboard state: apps using `GetAsyncKeyState`, DirectInput, or Raw Input may not recognize background holding.

### A key isn't being caught / "unmapped" in the log

You need the key's HID Usage ID. Run `probe_device.exe` after the Zadig swap — no wait, that one deliberately doesn't do a test read. Instead, temporarily add debug logging to `ProcessReport` (or run with logging and watch `sidekick.log`), press the key, and read byte index 2 of the raw report:

```
R: 00 00 64 00 00 00 00 00
        ^^--- Usage ID 0x64
```

Then add `USAGE_64={YourKey}` to the profile's `[...Keys]` section. Full table in [HID-USAGE-TABLE.md](HID-USAGE-TABLE.md).

### After hibernate/sleep, the keyboard doesn't work

The router auto-reconnects on resume via `WM_POWERBROADCAST`. If it didn't:
- Check `sidekick.log` for `Power resume → reconnect` / `Reconnected`.
- If the device disappeared entirely (some hubs renumber on resume), the router retries for ~10s. Restart `sidekick.exe` if needed.
- Worst case the device path changed; re-run `probe_device.exe` and confirm the GUID in the source still matches. (The GUID is usually stable across reboots.)

### A modifier got stuck (Ctrl / Shift / Alt held down)

This can happen if the router is forcibly killed mid-press before it can send the matching key-up. Press and release the stuck key on a physical keyboard to clear it. On normal profile switch, reload, disconnect, suspend, or clean exit, KeySidekick releases only the basic/targeted keys it owns.

## Configuration

### How do I find my target app's window class?

**Spy++** (ships with Visual Studio): Spy → Find Window, drag the crosshair onto the app's window, read "Class".

**PowerShell** (no install):
```powershell
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W { [DllImport("user32.dll")] public static extern IntPtr FindWindowW(string c,string n); }
"@
# by class name you already know — verify it exists:
$h = [W]::FindWindowW("TAIMPMainForm", $null)
if ($h -ne [IntPtr]::Zero) { "found: $h" } else { "not found" }
```

Or enumerate all top-level windows with their classes and grep:
```powershell
Get-Process | Where-Object { $_.MainWindowTitle } | ForEach-Object {
  Add-Type -Namespace U -Name A -MemberDefinition '... GetClassName ...'   # see Win32 GetClassNameW
}
```
The simplest reliable path: search the web for "<app name> window class name" — most popular apps are documented.

### How do I map a Russian / non-US layout key?

You don't map by character — **HID Usage IDs are physical key positions, layout-independent.** The physical key labelled `Й` on a Russian keyboard is Usage `0x14` (same physical position as `Q` on US QWERTY). Map by Usage ID, not by what's printed on the key. Full table: [HID-USAGE-TABLE.md](HID-USAGE-TABLE.md).

### Can one profile send different keys to different apps?

Yes — that's multi-app routing via the `!app:` token:
```ini
[Profile.mixer.Keys]
USAGE_14={F1}                              ; Q → profile target (e.g. AIMP)
USAGE_08=!app:Spotify:{Media_Play_Pause}   ; E → Spotify (different app, same profile)
USAGE_15=!app:VLC:{F9}                     ; R → VLC
```
`!app:<nameOrClass>:<keys>` resolves `<nameOrClass>` first as a window class, then as another profile's name (uses that profile's `TargetClass`).

### Can I switch profiles with Ctrl+Shift+1 etc.?

Yes — key combinations (modifier + key):
```ini
USAGE_1E+Ctrl+Shift=!switch:aimp    ; Ctrl+Shift+1 → aimp
USAGE_1E=!switch:basic              ; plain 1 → basic (coexists with the combo)
```
Modifier names: `Ctrl Shift Alt Win` (any side), or specific `LCtrl RCtrl LShift RShift LAlt RAlt LWin RWin`. Combine with `+`. The combo fires when *all* required modifier groups are held; a bare-key mapping without modifiers still works when no modifiers are held.

## Limitations

### Basic mode doesn't work in my game / gets me kicked by anti-cheat

This is a hard Windows limitation, not a bug. `SendInput` (used by basic mode) tags every event with `LLKHF_INJECTED`. Anti-cheat (EAC, BattlEye, Vanguard) and many games inspect this flag and ignore or flag injected input — there is no user-mode workaround.

Options:
- Use a `targeted` profile instead (sends via `PostMessage`, but many games ignore window messages too).
- For true game compatibility, the keyboard must stay on the native HID driver. Revert the Zadig swap (Device Manager → uninstall → scan for hardware changes) and don't use this tool for that game.
- A kernel-level virtual HID driver (like [FakerInput](https://github.com/monkeym4ster/FakerInput) or a hardware macro pad) is the only way to inject "real-looking" input, and that's out of scope here.

### Can it tell two identical keyboards apart?

If both have the same VID/PID, `DeviceVIDPID` substring matching will pick whichever the SetupAPI enumeration returns first — not reliable. Workarounds: use the more specific device instance path, or plug only one of them in. (Per-instance matching by full path is a future feature.)

## Troubleshooting

### Enable verbose logging

In `config.ini`:
```ini
[General]
EnableLog=1
```
Then watch `sidekick.log`. Every key dispatch, profile switch, and device reconnect is logged. To see raw HID reports, temporarily add a `Log()` call at the top of `ProcessReport` in `sidekick.cpp` and rebuild.
