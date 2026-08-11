# HID Keyboard Usage IDs (USB HID Usage Table, page 0x07)

The HID keyboard report (bytes 2–7 of the standard 8-byte report) contains **Usage IDs** from HID Usage Page 0x07 (Keyboard/Keypad). These are **physical key positions**, independent of keyboard layout (QWERTY/AZERTY, language). Usage `0x14` is always the physical Q-position — whether it types Q, Й, or A depends on the active layout, which we bypass by mapping Usage IDs directly.

Use these hex values in the router INI: `USAGE_<hex> = <key string>`.

## Modifier bits (byte 0 of the report, not a Usage ID — for reference)

| Bit | Modifier |
|---|---|
| 0x01 | Left Ctrl |
| 0x02 | Left Shift |
| 0x04 | Left Alt |
| 0x08 | Left GUI (Win) |
| 0x10 | Right Ctrl |
| 0x20 | Right Shift |
| 0x40 | Right Alt |
| 0x80 | Right GUI (Win) |

## Letters and numbers (the common mappings)

| Usage | Key (QWERTY position) | | Usage | Key |
|---|---|---|---|---|
| 0x04 | a | | 0x13 | p |
| 0x05 | b | | 0x14 | q |
| 0x06 | c | | 0x15 | r |
| 0x07 | d | | 0x16 | s |
| 0x08 | e | | 0x17 | t |
| 0x09 | f | | 0x18 | u |
| 0x0A | g | | 0x19 | v |
| 0x0B | h | | 0x1A | w |
| 0x0C | i | | 0x1B | x |
| 0x0D | j | | 0x1C | y |
| 0x0E | k | | 0x1D | z |
| 0x0F | l | | | |
| 0x10 | m | | 0x1E | 1 |
| 0x11 | n | | 0x1F | 2 |
| 0x12 | o | | 0x20 | 3 |
| | | | 0x21 | 4 |
| | | | 0x22 | 5 |
| | | | 0x23 | 6 |
| | | | 0x24 | 7 |
| | | | 0x25 | 8 |
| | | | 0x26 | 9 |
| | | | 0x27 | 0 |

## Function keys

| Usage | Key | | Usage | Key |
|---|---|---|---|---|
| 0x3A | F1 | | 0x41 | F8 |
| 0x3B | F2 | | 0x42 | F9 |
| 0x3C | F3 | | 0x43 | F10 |
| 0x3D | F4 | | 0x44 | F11 |
| 0x3E | F5 | | 0x45 | F12 |
| 0x3F | F6 | | 0x46 | PrintScreen |
| 0x40 | F7 | | 0x47 | ScrollLock |

## Navigation / editing

| Usage | Key | | Usage | Key |
|---|---|---|---|---|
| 0x28 | Enter | | 0x49 | Insert |
| 0x29 | Esc | | 0x4A | Home |
| 0x2A | Backspace | | 0x4B | PageUp |
| 0x2B | Tab | | 0x4C | Delete |
| 0x2C | Space | | 0x4D | End |
| 0x2D | - (minus) | | 0x4E | PageDown |
| 0x2E | = (equals) | | 0x4F | RightArrow |
| 0x2F | [ | | 0x50 | LeftArrow |
| 0x30 | ] | | 0x51 | DownArrow |
| 0x31 | \ (backslash) | | 0x52 | UpArrow |
| 0x33 | ; (semicolon) | | | |
| 0x34 | ' (apostrophe) | | | |
| 0x35 | ` (grave accent) | | | |
| 0x36 | , (comma) | | | |
| 0x37 | . (period) | | | |
| 0x38 | / (slash) | | | |
| 0x39 | CapsLock | | | |

## Modifier keys (as Usage IDs, rarely in the array — usually in byte 0)

| Usage | Key |
|---|---|
| 0xE0 | Left Ctrl |
| 0xE1 | Left Shift |
| 0xE2 | Left Alt |
| 0xE3 | Left GUI |
| 0xE4 | Right Ctrl |
| 0xE5 | Right Shift |
| 0xE6 | Right Alt |
| 0xE7 | Right GUI |

## Numpad

| Usage | Key | | Usage | Key |
|---|---|---|---|---|
| 0x53 | NumLock | | 0x5A | Numpad Enter |
| 0x54 | Numpad / | | 0x5B | Numpad 1 |
| 0x55 | Numpad * | | 0x5C | Numpad 2 |
| 0x56 | Numpad - | | 0x5D | Numpad 3 |
| 0x57 | Numpad + | | 0x5E | Numpad 4 |
| 0x58 | Numpad Enter | | 0x5F | Numpad 5 |
| 0x59 | Numpad 1 | | 0x60 | Numpad 6 |
| 0x5A | Numpad 2 | | 0x61 | Numpad 7 |
| ... | (see HID spec) | | 0x62 | Numpad 8 |
| | | | 0x63 | Numpad 9 |
| | | | 0x62 | Numpad 0 |

(Some numpad codes overlap with navigation when NumLock is off — check the full HID spec for exact values.)

## Multi-media / consumer keys

Standard keyboard Usage Page (0x07) does **not** cover media keys (Play, Volume, etc.). Those live on Usage Page 0x0C (Consumer Controls), typically delivered through a **different HID interface** (e.g. `MI_01&COL02` consumer-control collection), not the keyboard interface. They will not appear in the keyboard report bytes 2–7. If you need them, you'd read the consumer-control interface separately — usually not worth it; map to F-keys instead.

## How to find an unknown Usage ID empirically

If a key isn't in this table, run the router (or `probe_device`) and press it — the console/log dumps the raw report:
```
R: 00 00 64 00 00 00 00 00
```
Byte index 2 (here `0x64`) is the Usage ID. Put `USAGE_64 = {KeyString}` in the INI.

## Reference

- Official spec: USB HID Usage Tables, https://usb.org/sites/default/files/hut1_22.pdf (Keyboard/Keypad page starts around p. 88).
- Modifier byte semantics: USB HID 1.11 spec, Appendix B.1.
