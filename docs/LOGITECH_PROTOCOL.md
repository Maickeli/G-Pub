# Logitech HID++ Interoperability Notes

These notes summarize the HID++ report shapes and command values GPub uses for Logitech onboard profile switching. They are written as interoperability notes, not copied upstream source code.

Reference project: [omm.py](https://github.com/lexr1/omm.py).

## Transport

- Logitech vendor id: `0x046D`
- HID++ 2.0 long report uses report id `0x11`
- Packet shape (20-byte long report):
  - byte 0: report id (`0x11`)
  - byte 1: device index (`0xFF` wired, `1..6` receiver, `0` bluetooth)
  - byte 2: feature index
  - byte 3: `(function_id << 4) | swid` (`0xF` is used here)
  - byte 4..: function params

Commands are written, then a matching response is read back and validated against request header bytes 0..3.

## Feature Discovery

HID++ calls use feature **indices**, not ids directly.

- Root feature id is `0x0000`
- Root function `0` resolves a feature id:
  - params: `[feature_id_hi, feature_id_lo]`
  - response byte 4: feature index (or invalid when unsupported)

Onboard profile feature id:

- `0x8100` (`onboard_profile`)

## Onboard Profile Commands (`feature 0x8100`)

Function ids used by the onboard profile feature:

- `0`: get onboard profile info
- `1`: set onboard mode (`1`=onboard, `2`=host/software)
- `2`: get onboard mode
- `3`: set current profile
- `4`: get current profile

Profile switch command payload:

- function `3`
- params `[0, profile_index, 0]` where `profile_index` is one-based.

## Practical Notes

- For wired devices, `device_index=255` is typical.
- For receiver devices, correct `device_index` is usually in `1..6`.
- If onboard mode is disabled, switching may require enabling onboard mode first.
