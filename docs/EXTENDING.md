# Extending GPub

GPub's current backend interface is centered on applying profile payloads. Try to keep new backend code flexible enough that it could later support profile editing, manual switching, battery state, connection state, or transport-specific details.

## Add a Device Backend
1. Implement `gpub::IDeviceBackend`.
2. Return a stable backend id in `id()` (example: `wooting`, `logitech`).
3. Keep `available()` cheap and non-blocking.
4. Implement `applyProfile(profile_name, payload)` using backend payload keys.
5. Optionally override `validate(payload)` for stricter config checks.
6. Register the backend in `src/core/backend_factory.cpp`.

For now, keep new backend payloads small and explicit. Avoid baking UI assumptions into backend code; a tray app should be able to reuse the same device transport code later.

Payloads are keyed by backend id in JSON:

```json
{
  "profiles": {
    "gaming": {
      "payloads": {
        "my_backend": {
          "some_key": "value"
        }
      }
    }
  }
}
```

## Add a Platform Foreground Provider
1. Implement `gpub::IForegroundWindowProvider`.
2. Ensure callback emissions are event-driven when possible.
3. Emit fully populated `ActiveWindowInfo` best-effort values.
4. Keep callback path short; defer heavy work and debounce.
5. Register provider in `src/core/platform_factory.cpp`.

## Maybe Later: Plugins
- Current backend/provider registration is static for low complexity.
- To add runtime plugins later, keep the same interfaces and load implementations behind factories.
- Preserve `id()` compatibility so existing configs stay valid.
