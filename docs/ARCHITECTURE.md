# GPub Architecture

## What This Is
GPub starts as an automatic profile switcher, but it should not stay limited to that. The code should be able to grow into a small tray app for controlling desk gear across vendors and transports: editing profiles, switching them manually, showing device status, reporting battery levels, and supporting devices that are not HID.

## Right Now
- Windows-first profile switching with a core that stays portable.
- Very low overhead for 24/7 background use.
- Event-driven focus handling to avoid polling loops.
- Keep device logic behind backend interfaces so later UI/status work can reuse it.

## Module Layout
- `include/gpub/`: data models and interfaces.
- `src/core/`: config loading, rules engine, orchestration, factories.
- `src/platform/windows/`: WinEvent hook provider and Win32 helpers.
- `src/platform/linux/` + `src/platform/macos/`: placeholders for later providers.
- `src/backends/`: backend implementations for Wooting HID and Logitech HID++.
- `src/cli/`: `gpubd` daemon process and `gpubctl` command tool.

## Runtime Flow
1. `gpubd` loads JSON config and builds core components.
2. Windows provider installs `SetWinEventHook(EVENT_SYSTEM_FOREGROUND)`.
3. Foreground events are coalesced with a one-shot debounce timer.
4. After debounce, provider resolves `ActiveWindowInfo`.
5. Core rules engine selects target profile.
6. Orchestrator skips unchanged profile applies.
7. Device writes are rate-limited and optionally deferred.
8. Backends apply payloads (or log-only in dry-run mode).

## Later
- A tray process can host the same core rules and backend registry.
- Backends can grow from write-only profile appliers into device controllers that expose editable profiles, battery level, connection state, and other useful device info where available.
- Automatic switching should stay as one feature, not become the whole shape of the project.
- CLI tools can stay useful for scripting and diagnostics even after a tray UI exists.

## Low Overhead Design Choices
- Hook-based foreground detection (`SetWinEventHook`) first.
- Polling only as fallback when hook setup fails (default 1000ms).
- WinEvent callback does no heavy work; it only queues HWND.
- Expensive process path/name query is cached by HWND/PID.
- Coalescing uses a single debounce timer (default 200ms).
- Orchestrator keeps one pending window event instead of unbounded queues.
- No repeated writes when profile is unchanged.
- Minimal dependencies (C++ standard library + Win32 APIs).

## Threading Model
- Provider runs a small Win32 message-loop thread for hook callbacks/timers.
- Orchestrator runs on the main thread.
- Provider callback enqueues latest window and returns quickly.
- Device apply happens in orchestrator flow with rate-limit guard.

## Shutdown
- `gpubd` handles console close/Ctrl+C signals.
- Orchestrator stops provider, exits event loop, and joins threads cleanly.
