# VaderHaptics

Windows haptics middleware for the Flydigi Vader 5 Pro. It presents a virtual
DualSense to compatible games, preserves the game feedback as long as possible,
then renders it to the Vader's two grip and two trigger vibration lanes.

> Experimental hardware software. It changes controller visibility only for an
> active PS5 Enhanced session and restores Native mode on exit or failure.

中文说明在 [README.zh-CN.md](README.zh-CN.md)。

## What it is / is not

- **PS5 Enhanced:** virtual DualSense input plus DualSense haptic rendering.
- **Native:** releases VaderHaptics and returns the controller to Flydigi.
- **Not included:** RGB, macros, keyboard/mouse mapping, stick curves,
  adaptive-trigger resistance, game UI patching, account services, or anti-cheat
  workarounds.

## Verified baseline

Windows 10/11 x64, Flydigi Vader 5 Pro over USB or the Flydigi 2.4G receiver,
with The Last of Us Part I as the owner-tested reference game. Bluetooth and
Xbox Enhanced are not supported. Other games with native USB DualSense haptics
may work, but are unverified.

## Quick start

1. Read [docs/INSTALL.zh-CN.md](docs/INSTALL.zh-CN.md) and install the required
   external dependencies yourself.
2. Build a release package with `powershell -ExecutionPolicy Bypass -File
   .\scripts\Build-Release.ps1`.
3. Run `VaderHaptics.exe` as administrator from the generated package.
4. A running Flydigi Space Station is detected automatically. Otherwise use
   **选择空间站目录…** (Choose Space Station folder); the path is remembered.
5. Use **Start PS5 Enhanced**. Use **Return Native** before changing Flydigi
   settings, disconnecting the controller, or troubleshooting.

## Development

Requirements: CMake 3.20+, Visual Studio 2022 C++ desktop tools, and a Windows
10/11 x64 host.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

See [CONTRIBUTING.md](CONTRIBUTING.md) and
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).
