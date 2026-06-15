# Configuration (JSON)

This config format is for the current automatic profile-switching setup. If GPub grows a tray UI later, foreground rules and backend payloads should still be useful for automation.

Config file is JSON with top-level keys:
- `global`: runtime behavior
- `profiles`: profile metadata + backend payloads
- `rules`: match-to-profile selection rules

## Global Keys
- `debounce_ms` (int): focus-change debounce (default 200)
- `device_rate_limit_ms` (int): minimum spacing between device apply calls (default 250)
- `fallback_poll_interval_ms` (int): low-rate polling interval when WinEvent hook fails (default 1000)
- `default_profile` (string): profile to use when no rule matches
- `log_level` (string): `error|warn|info|debug`

## Rule Matching
Rule fields:
- `profile` (string, required)
- `priority` (int, optional, default 0)
- `executable_path` (string, optional)
- `process_name` (string, optional)
- `window_title_regex` (string, optional)

Selection order:
1. Higher `priority` wins.
2. If same priority, more specific rule wins:
   - executable path match > process name match > title regex match.
3. If still tied, first rule in config wins.

## Example
See `config/examples/config.json`.

## Wooting Profile Payload
For the current Wooting backend implementation, set:
- `profiles.<name>.payloads.wooting.profile_index`: string/integer `1..4`

Example:
```json
{
  "profiles": {
    "default": {
      "payloads": {
        "wooting": {
          "profile_index": "1"
        }
      }
    },
    "gaming": {
      "payloads": {
        "wooting": {
          "profile_index": "2"
        }
      }
    }
  }
}
```

## Logitech Onboard Profile Payload
For Logitech HID++ 2.0 mice with onboard memory:
- `profiles.<name>.payloads.logitech.profile_index`: string/integer `1..9`
- `profiles.<name>.payloads.logitech.device_index`: optional HID++ device index
  - `255` for wired (common)
  - `1..6` for receiver slots
  - `0` for bluetooth
  - if omitted, GPub probes common indices automatically
- `profiles.<name>.payloads.logitech.pid`: optional USB product id filter, hex or decimal (example `0xC08B`)
- `profiles.<name>.payloads.logitech.name_contains`: optional case-insensitive product name substring filter
- `profiles.<name>.payloads.logitech.force_onboard_mode`: optional boolean (`true`/`false`), default `true`

Example:
```json
{
  "profiles": {
    "default": {
      "payloads": {
        "logitech": {
          "profile_index": "1",
          "force_onboard_mode": "true"
        }
      }
    },
    "gaming": {
      "payloads": {
        "logitech": {
          "profile_index": "2",
          "force_onboard_mode": "true"
        }
      }
    }
  }
}
```
