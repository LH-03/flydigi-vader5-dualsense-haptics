#pragma once

#include <cstdint>

// Vader 5 Pro raw hat (data[11] & 0x0F) is a 4-direction bitmask, verified by
// owner physical observation 2026-08-12:
//   rest = 0, up = 0x01, right = 0x02, down = 0x04, left = 0x08
//   diagonals are bitwise ORs (up-right = 0x03, down-right = 0x06,
//   down-left = 0x0C, up-left = 0x09).
// DS5 D-pad bitmask (VIIPER payload byte 8): up = 0x01, down = 0x02,
// left = 0x04, right = 0x08.
inline uint8_t MapVaderHatToDs5Dpad(uint8_t hat)
{
    uint8_t out = 0;
    if (hat & 0x01) {
        out |= 0x01; // raw up -> DS5 up
    }
    if (hat & 0x04) {
        out |= 0x02; // raw down -> DS5 down
    }
    if (hat & 0x08) {
        out |= 0x04; // raw left -> DS5 left
    }
    if (hat & 0x02) {
        out |= 0x08; // raw right -> DS5 right
    }
    return out;
}
