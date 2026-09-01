#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace vaderhaptics {

struct MarkerInputState {
    int16_t lx = 0;
    int16_t ly = 0;
    uint8_t l2 = 0;
    uint8_t r2 = 0;
    uint16_t face = 0;
};

class CaptureInputMarkerDetector {
public:
    static constexpr uint8_t kTriggerThreshold = 16;
    static constexpr uint8_t kTriggerRearm = 8;
    static constexpr int kStickStartThreshold = 6000;
    static constexpr int kStickStopThreshold = 3000;
    static constexpr uint16_t kFaceMarkerMask = 0x01B0;

    struct Edges {
        std::array<const char*, 8> names = {};
        size_t count = 0;

        void Add(const char* name)
        {
            if (count < names.size()) {
                names[count++] = name;
            }
        }
    };

    Edges Observe(const MarkerInputState& state)
    {
        Edges edges;
        const bool r2Above = state.r2 >= kTriggerThreshold;
        const bool l2Above = state.l2 >= kTriggerThreshold;
        const int leftX = state.lx < 0 ? -state.lx : state.lx;
        const int leftY = state.ly < 0 ? -state.ly : state.ly;
        const bool stickStart = leftX >= kStickStartThreshold ||
            leftY >= kStickStartThreshold;
        const bool stickStop = leftX < kStickStopThreshold &&
            leftY < kStickStopThreshold;

        if (!initialized_) {
            // Capture may begin while a trigger or stick is already held.
            // Treat that first report as the baseline and require a real
            // release below the re-arm threshold before reporting a trigger
            // rise. This prevents a held trigger from becoming a synthetic
            // edge on the second observed report.
            initialized_ = true;
            r2Armed_ = state.r2 < kTriggerRearm;
            l2Armed_ = state.l2 < kTriggerRearm;
            leftStickMoving_ = stickStart;
            lastFace_ = state.face;
            return edges;
        }

        if (state.r2 < kTriggerRearm) {
            r2Armed_ = true;
        }
        else if (r2Above && r2Armed_) {
            edges.Add("input.rt_rise");
            r2Armed_ = false;
        }
        if (state.l2 < kTriggerRearm) {
            l2Armed_ = true;
        }
        else if (l2Above && l2Armed_) {
            edges.Add("input.lt_rise");
            l2Armed_ = false;
        }

        const uint16_t risingFaces =
            static_cast<uint16_t>((~lastFace_) & state.face & kFaceMarkerMask);
        if (risingFaces & 0x0010) edges.Add("input.face_cross_rise");
        if (risingFaces & 0x0020) edges.Add("input.face_circle_rise");
        if (risingFaces & 0x0080) edges.Add("input.face_square_rise");
        if (risingFaces & 0x0100) edges.Add("input.face_triangle_rise");

        if (!leftStickMoving_ && stickStart) {
            edges.Add("input.left_stick_move_start");
            leftStickMoving_ = true;
        }
        else if (leftStickMoving_ && stickStop) {
            edges.Add("input.left_stick_move_stop");
            leftStickMoving_ = false;
        }

        lastFace_ = state.face;
        return edges;
    }

private:
    bool initialized_ = false;
    bool r2Armed_ = false;
    bool l2Armed_ = false;
    bool leftStickMoving_ = false;
    uint16_t lastFace_ = 0;
};

} // namespace vaderhaptics
