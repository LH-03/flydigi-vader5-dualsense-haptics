#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace vaderhaptics {

enum class VpcmError {
    None = 0,
    BadMagic,
    BadVersion,
    BadLength,
    UnknownType,
    CrcMismatch,
    SequenceGap,
};

struct VpcmFrame {
    uint32_t generation = 0;
    uint8_t type = 0;
    uint32_t sequence = 0;
    std::vector<uint8_t> payload; // full validated payload
};

class VpcmParser {
public:
    using FrameCallback = std::function<void(const VpcmFrame&)>;
    using ErrorCallback = std::function<void(VpcmError)>;

    VpcmParser(FrameCallback onFrame, ErrorCallback onError);

    // Feed arbitrary TCP chunk; may publish zero, one or many frames.
    void Feed(const uint8_t* data, size_t size);

    // Reconnect boundary: discard partial bytes and re-arm sequence tracking.
    void ResetGeneration();

    uint32_t generation() const { return generation_; }
    uint64_t frameCount() const { return frameCount_; }

    uint64_t errorCount(VpcmError error) const;

private:
    void ReportError(VpcmError error);
    void EmitFrame(uint8_t type, uint32_t sequence,
        const uint8_t* payload, size_t payloadSize);

    FrameCallback onFrame_;
    ErrorCallback onError_;

    uint32_t generation_ = 1;
    uint64_t frameCount_ = 0;
    std::vector<uint8_t> partial_;
    bool sequenceInitialized_ = false;
    uint32_t expectedSequence_ = 0;

    uint64_t badMagic_ = 0;
    uint64_t badVersion_ = 0;
    uint64_t badLength_ = 0;
    uint64_t unknownType_ = 0;
    uint64_t crcMismatch_ = 0;
    uint64_t sequenceGaps_ = 0;
};

} // namespace vaderhaptics
