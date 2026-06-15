# GPub

Windows-first device control project for keyboards, mice, headsets, and other desk gear.

Right now GPub automatically switches keyboard/mouse profiles when the active app changes. Over time, the idea is to turn it into a small tray app for editing profiles, switching them manually or automatically, and showing useful device info like battery level and connection state.

## Right Now
- Automatic profile switching from foreground-window rules.
- Wooting keyboard profile switching over HID.
- Logitech HID++ onboard mouse profile switching.
- CLI/daemon workflow while the tray UI does not exist yet.

## Later
- Tray app with quick manual profile switching.
- Profile editing and device status views.
- Battery and connection status for devices that expose it.
- Additional device classes, including headsets and non-HID transports.

## Project Layout
- `include/gpub/`: core interfaces and data models.
- `src/core/`: config loading, rules, orchestration, and factories.
- `src/backends/`: device backends for Wooting HID and Logitech HID++.
- `src/platform/windows/`: foreground-window detection via Win32.
- `src/cli/`: daemon and control CLI.
- `config/examples/`: sample runtime configuration.
- `docs/`: architecture, config, extension, and protocol notes.

## Binaries
- `gpubd`: background daemon process.
- `gpubctl`: CLI helper (`status`, `reload` validation stub, `test-match`).

## Build (Visual Studio generator)
```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## Build (Ninja + MSVC/clang-cl)
```bat
cmake -S . -B build-ninja -G Ninja
cmake --build build-ninja
```

## Run
```bat
.\build\Release\gpubd.exe --config config\examples\config.json --dry-run
```

Run detached in background (no terminal output/window):
```bat
.\build\Release\gpubd.exe --config config\examples\config.json --background --quiet
```

Example config behavior:
- `ExampleGame.exe` focused -> Wooting profile slot `2`
- `ExampleGame.exe` focused -> Logitech onboard slot `2`
- any other window -> Wooting slot `1` and Logitech slot `1`

## Test Rule Match
```bat
.\build\Release\gpubctl.exe test-match --config config\examples\config.json --exe "C:\Games\ExampleGame\ExampleGame.exe" --title "Example Game"
```

See:
- `docs/ARCHITECTURE.md`
- `docs/CONFIG.md`
- `docs/EXTENDING.md`
- `docs/WOOTING_PROTOCOL.md`
- `docs/LOGITECH_PROTOCOL.md`

## Dependency Notes
GPub does not vendor Python tooling or upstream Wooting/Logitech repositories.
The active Wooting code lives in `src/backends/wooting/`.
Protocol findings from upstream projects are captured as concise notes in `docs/`.
