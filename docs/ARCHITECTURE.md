# Architecture

VaderHaptics has two user-facing states:

- **Native:** no renderer or virtual controller remains after an explicit zero
  frame and ownership release.
- **PS5 Enhanced:** `Vader input -> virtual DualSense -> game -> VIIPER
  feedback -> DS5 renderer -> Vader four-lane output`.

The key boundary is:

`Haptic Source -> Renderer -> VaderHapticFrame -> Physical Output Backend`

`VaderHapticFrame` carries left grip, right grip, left trigger, and right
trigger values. Rich DS5 haptic PCM/effect information remains available to the
renderer before this final four-lane boundary.

The runtime dynamically resolves the supported Vader topology, uses HidHide
only during PS5 Enhanced to prevent double input, and persists/validates a
Native restoration path. Feedback queues are bounded and newest-wins. An input
outage or physical write failure must send zero and restore Native rather than
replaying stale state.

The renderer has no game-specific code. Game compatibility is a property of the
virtual DualSense route and the game’s own haptic output.
