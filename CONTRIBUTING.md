# Contributing

Thank you for helping improve VaderHaptics.

## Guardrails

- Keep the project haptics-only. Do not add general remapping, macros,
  keyboard/mouse injection, RGB, cloud services, or anti-cheat workarounds.
- Keep input routing, haptic source, renderer, and physical output separate.
- A stale haptic event is worse than a dropped one. Preserve bounded queues and
  explicit zero/recovery behavior.
- Never commit controller profiles, game files, captures, firmware, third-party
  binaries, personal paths, credentials, or hardware identifiers.
- Do not claim hardware/game compatibility without repeatable owner evidence.

## Pull requests

1. Discuss a new backend, public contract, dependency, or device-visibility
   approach in an issue first.
2. Keep changes narrow and add an offline test when practical.
3. Run the commands in the README and include the exact result.
4. Describe tested hardware, connection mode, game, and how Native recovery was
   verified. Do not include personal logs verbatim.

Security-sensitive issues belong in `SECURITY.md`, not public issue trackers.
