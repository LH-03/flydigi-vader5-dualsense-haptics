# External dependencies

VaderHaptics intentionally does not vendor drivers or virtual-controller
binaries.

| Dependency | Why it is required | Installation boundary |
|---|---|---|
| Flydigi Space Station | Configures the controller and provides the onboard profile that is snapshotted at enhanced-mode start. | Install normally from Flydigi. Enable its third-party mapping takeover setting, then set its directory in `VaderHapticsLauncher.ini`. |
| HidHide | Makes the physical XInput/HID device invisible to games during PS5 Enhanced, avoiding duplicate input. | Install the official signed x64 release. The current runtime expects `HidHideCLI.exe` under `C:\Program Files\Nefarius Software Solutions\HidHide\x64\`. |
| VIIPER | Provides the virtual USB DualSense and feedback transport. | Install a release explicitly marked compatible by VaderHaptics release notes. The runtime defaults to `%LOCALAPPDATA%\VIIPER\viiper.exe`. Do not update it while a compatibility test is in progress. |

The reference implementation was validated with a VIIPER v0.0.9-compatible
runtime. Later releases are not automatically assumed compatible. If the
runtime reports a different API/device behavior, stay Native and report the
version and a redacted log excerpt.

The first use of the launcher requires administrator approval because HidHide
changes device visibility. The launcher’s Native transition must be allowed to
finish before uninstalling either external dependency.
