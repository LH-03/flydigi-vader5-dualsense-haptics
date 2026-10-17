# External dependencies

VaderHaptics intentionally does not vendor drivers or virtual-controller
binaries.

| Dependency | Why it is required | Installation boundary |
|---|---|---|
| Flydigi Space Station | Configures the controller and provides the onboard profile that is snapshotted at enhanced-mode start. | Install normally from Flydigi. Enable its third-party mapping takeover setting, then set its directory in `VaderHapticsLauncher.ini`. |
| HidHide | Makes the physical XInput/HID device invisible to games during PS5 Enhanced, avoiding duplicate input. | Install the official signed x64 release. The current runtime expects `HidHideCLI.exe` under `C:\Program Files\Nefarius Software Solutions\HidHide\x64\`. |
| VIIPER | Provides the virtual USB DualSense and feedback transport. | Build the pinned input-fixed runtime with `scripts/Build-Viiper.ps1` (Go 1.26.2 and Git), then keep its `runtime` directory beside the launcher/bridge. The ordinary unpatched v0.0.9 binary is not sufficient. |

The supported runtime is pinned to `3a7f183699d4b508b028abfd17889fb189cd7950`
plus the input queue and lifecycle patch in `third-party/viiper`. It preserves
press/release transitions in a bounded capacity-8 queue, isolates audio endpoint
resets from input, and neutralizes input on stream end. A capability marker
lets the bridge reject unpatched runtimes before playing; a matching version
number alone does not prove compatibility. Later versions are not assumed safe.

Build this runtime before `scripts/Build-Release.ps1`. The generated runtime
directory contains its GPL license, pinned source archive and source patches.
Retain those materials with any redistributed runtime binary. This does not
replace the machine's installed VIIPER or modify driver installations.

The first use of the launcher requires administrator approval because HidHide
changes device visibility. The launcher’s Native transition must be allowed to
finish before uninstalling either external dependency.
