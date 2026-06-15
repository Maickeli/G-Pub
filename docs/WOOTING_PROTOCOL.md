# Wooting HID Interoperability Notes

These notes summarize the HID report shapes and command values GPub uses for Wooting profile switching. They are written as interoperability notes, not copied upstream source code.

Reference projects:

- Wooting RGB SDK: <https://github.com/WootingKb/wooting-rgb-sdk>
- ShayBox Wooting Profile Switcher: <https://github.com/ShayBox/Wooting-Profile-Switcher>

## Discovery

Wooting RGB SDK enumerates Wooting devices and selects HID interfaces where usage page is:

- `0x1337` (legacy config interface), or
- `0xFF55` (multi-report interface; e.g., 60HE v2 family).

For `0xFF55`, the SDK sets `uses_multi_report = true`.

## Feature Command Frame

The reference RGB SDK sends an 8-byte feature report for these commands.

8-byte feature report:

- Byte `0`: report ID (`1` for multi-report, otherwise `0`)
- Byte `1`: magic (`0xD1` for multi-report, otherwise `0xD0`)
- Byte `2`: magic suffix (`0xDA`)
- Byte `3`: command ID
- Byte `4`: parameter3
- Byte `5`: parameter2
- Byte `6`: parameter1
- Byte `7`: parameter0

Important: parameter order is reversed into bytes 4..7 (`p3, p2, p1, p0`).

## Profile Commands

Command values observed in the reference profile-switching flow:

- `ACTIVATE_PROFILE = 23`
- `RELOAD_PROFILE = 7`
- `GET_CURRENT_KEYBOARD_PROFILE_INDEX = 11`
- `REFRESH_RGB_COLORS = 29`
- `WOOT_DEV_RESET_ALL = 32`

## Switching Sequence Used by Profile Switcher

`set_active_profile_index(profile_index, send_sleep_ms, swap_lighting)`:

1. Send `ACTIVATE_PROFILE (23)` with `parameter0 = profile_index`
2. Sleep `send_sleep_ms`
3. Send `RELOAD_PROFILE (7)` with `parameter0 = profile_index`
4. If `swap_lighting`:
   - Sleep, send `WOOT_DEV_RESET_ALL (32, p0=0)`
   - Sleep, send `REFRESH_RGB_COLORS (29, p0=profile_index)`

`profile_index` is zero-based in ShayBox (tray index `0..n-1`).

## Response Behavior

RGB SDK `wooting_usb_send_feature(...)` sends the feature command, then reads and discards a response via HID read timeout.

Response size chosen by SDK:

- v1: `128`
- v2: `256`
- multi-report: `2046`

## Operational Constraints

- Wootility and third-party switchers compete for the same HID interface.
- In practice, avoid running Wootility while using external profile switching.
