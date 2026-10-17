# Third-party and trademark notice

VaderHaptics is an independent project and is not affiliated with Flydigi,
Sony, Valve, Nefarius Software Solutions, or VIIPER's authors.

The public repository deliberately does **not** include any third-party binary,
controller profile, Flydigi configuration, game asset, capture, firmware, or
reverse-engineering output.

- [VIIPER](https://github.com/Alia5/VIIPER) is an external virtual-controller
  service used by the supported PS5 path. It is GPL-3.0-or-later and is not
  linked into this project. The separate reproducible input-fixed runtime build
  includes the upstream GPL license, pinned source archive and patches. Keep
  those materials with the binary when distributing a generated package;
  do not replace it with a matching-version stock binary or automatic update.
- [HidHide](https://github.com/nefarius/HidHide) is an external device
  visibility driver. Install it from the official signed release. It is not
  bundled or modified by this project.
- SDL was used as a protocol reference. No SDL source is included in this
  export.

See `docs/DEPENDENCIES.md` before distributing a binary release.
