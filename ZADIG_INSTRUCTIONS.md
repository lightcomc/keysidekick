# Zadig: swapping your keyboard's MI_00 interface to WinUSB

## Goal

Detach the **keyboard interface (`MI_00`)** of *your* wireless keyboard (the composite
device whose VID/PID you wrote into `DeviceVIDPID`, e.g. `vid_xxxx&pid_yyyy`)
from the standard Windows keyboard stack and switch it to **Microsoft WinUSB**.
Afterwards the device no longer types into windows on its own, but becomes readable
directly by KeySidekick.

> Replace `VID_xxxx` / `PID_yyyy` everywhere below with **your** keyboard's values
> (Device Manager → the keyboard device → Details → *Hardware Ids*).

**IMPORTANT:** change **only `MI_00` (keyboard)**. Do **not** touch `MI_01` — on many
composite keyboards that is the mouse/touchpad and must keep working.

---

## Step 1. Download Zadig

1. Open <https://zadig.akeo.ie/>
2. Download **Zadig 2.9** (or newer) for Windows
3. Extract `zadig-2.9.exe` anywhere (e.g. `C:\Tools\`)
4. **Run it as administrator** (right-click → Run as administrator)

---

## Step 2. Show all devices

In the Zadig window:

1. Menu **Options → List All Devices** ✓ (check it)
2. Menu **Options → Drop Down** → make sure **Ignore Hubs or Composite Parents**
   is NOT checked (uncheck it if it is)

---

## Step 3. Find the target interface

In the device dropdown, find the entry that corresponds to the **keyboard
interface MI_00** of *your* device (`VID_xxxx&PID_yyyy`).

It will look something like:
```
USB Input Device (VID xxxx PID yyyy) [MI 00]    <- THE ONE YOU NEED
```

or
```
HID Keyboard Device (VID xxxx PID yyyy) [MI 00]
```

**How to tell it apart from the touchpad (`MI_01`):**
- `MI 00` = keyboard (your target interface) ← PICK THIS ONE
- `MI 01` = touchpad/mouse (do NOT touch!)

Double-check that the name really says `[MI 00]`.

⚠️ **If you pick `MI_01` by mistake you will break the touchpad.** Be careful.

---

## Step 4. Replace the driver

1. Select the right device (MI_00) in the dropdown
2. Below the list is the target driver. It must be:
   ```
   WinUSB (v6.1.7600.16385)    or newer
   ```
   If it shows something else (libusb, libusbK), use the ▲▼ arrows to select **WinUSB**
3. Click **Replace Driver** (or **Install Driver**)
4. Confirm the warning
5. Wait for it to finish (progress bar, ~10-30 seconds)

**Phantom-copy gotcha:** sometimes the old "keyboard" entry stays in the list and the
swapped device appears as a *second* copy. If the keyboard **still types normally**
after the swap, you swapped a phantom copy — run Zadig again, pick the entry that is
**not** on WinUSB yet, and click **Replace Driver** on it (do not pick the entry that
already says WinUSB).

---

## Step 5. Verify the result

1. Close Zadig
2. Open **Device Manager** (Win+X → Device Manager)
3. `VID_xxxx&PID_yyyy&MI_00` should now appear under **Universal Serial Bus devices**
   (or **libusb-win32 devices**) as "WinUSB Device" (instead of the former "Keyboard")
4. The `MI_01` touchpad must still be under **Mice** and working

---

## Lock check (before starting the app)

After the driver swap:
- Press keys on the target keyboard — they must **not** type into any window
  (the device is no longer a system keyboard)
- Your main keyboard works as usual
- The touchpad works as usual

If so, the swap succeeded. Now run `probe_device.exe`, then `sidekick.exe`
(see README: KeySidekick only reads the keyboard while `sidekick.exe` is running).

---

## Rollback (if something went wrong)

1. Device Manager → find `VID_xxxx&PID_yyyy&MI_00` (now under USB devices)
2. Right-click → **Uninstall device** ✓ "Delete the driver software for this device"
3. Menu **Action → Scan for hardware changes**
4. Windows reinstalls the standard keyboard driver

Alternative — open Zadig again, select the device, and install the **HID USB Driver**
instead of WinUSB.
