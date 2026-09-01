#include "m2/vpcm_parser.h"

#include <cstring>

namespace vaderhaptics {
namespace {

constexpr size_t kHeaderSize = 16;
constexpr size_t kFeedbackObjectSize = 474;
constexpr size_t kAtomicPayloadSize = 2 + kFeedbackObjectSize + 1920; // 2396
constexpr uint8_t kVersion = 0x05;
constexpr uint8_t kTypeFeedback = 0x81;
constexpr uint8_t kTypeAtomicMedia = 0x83;
constexpr uint8_t kTypeRealtimeHaptics = 0x84;

uint32_t Crc32(const uint8_t* data, size_t size)
{
    static uint32_t table[256] = {};
    static bool initialized = false;
    if (!initialized) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t crc = i;
            for (int j = 0; j < 8; ++j) {
                crc = (crc & 1) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
            }
            table[i] = crc;
        }
        initialized = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc = (crc >> 8) ^ table[(crc ^ data[i]) & 0xFFu];
    }
    return crc ^ 0xFFFFFFFFu;
}

uint32_t Load32LE(const uint8_t* data)
{
    return static_cast<uint32_t>(data[0]) |
        (static_cast<uint32_t>(data[1]) << 8) |
        (static_cast<uint32_t>(data[2]) << 16) |
        (static_cast<uint32_t>(data[3]) << 24);
}

uint16_t Load16LE(const uint8_t* data)
{
    return static_cast<uint16_t>(data[0]) |
        (static_cast<uint16_t>(data[1]) << 8);
}

} // namespace

VpcmParser::VpcmParser(FrameCallback onFrame, ErrorCallback onError)
    : onFrame_(std::move(onFrame)), onError_(std::move(onError))
{
}

void VpcmParser::Feed(const uint8_t* data, size_t size)
{
    if (size == 0) {
        return;
    }
    partial_.insert(partial_.end(), data, data + size);

    while (partial_.size() >= kHeaderSize) {
        const uint8_t* header = partial_.data();
        if (std::memcmp(header, "VPCM", 4) != 0) {
            ReportError(VpcmError::BadMagic);
            partial_.erase(partial_.begin());
            continue;
        }
        if (header[4] != kVersion) {
            ReportError(VpcmError::BadVersion);
            partial_.erase(partial_.begin());
            continue;
        }

        const uint8_t type = header[5];
        const uint16_t length = Load16LE(header + 6);
        const uint32_t sequence = Load32LE(header + 8);

        size_t expectedLength = 0;
        if (type == kTypeFeedback) {
            expectedLength = kFeedbackObjectSize;
        }
        else if (type == kTypeAtomicMedia) {
            expectedLength = kAtomicPayloadSize;
        }
        else if (type == kTypeRealtimeHaptics) {
            expectedLength = kFeedbackObjectSize;
        }
        else {
            ReportError(VpcmError::UnknownType);
            partial_.erase(partial_.begin());
            continue;
        }

        if (length != expectedLength) {
            ReportError(VpcmError::BadLength);
            partial_.erase(partial_.begin());
            continue;
        }

        if (partial_.size() < kHeaderSize + length) {
            break; // wait for the rest of the frame
        }

        const uint8_t* payload = partial_.data() + kHeaderSize;
        const uint32_t expectedCrc = Load32LE(header + 12);
        uint8_t crcInput[8 + kAtomicPayloadSize];
        std::memcpy(crcInput, header + 4, 8);
        std::memcpy(crcInput + 8, payload, length);
        const uint32_t actualCrc = Crc32(crcInput, 8 + length);
        if (actualCrc != expectedCrc) {
            ReportError(VpcmError::CrcMismatch);
            partial_.erase(partial_.begin(), partial_.begin() + kHeaderSize + length);
            continue;
        }

        if (sequenceInitialized_ && sequence != expectedSequence_) {
            ReportError(VpcmError::SequenceGap);
        }
        expectedSequence_ = sequence + 1;
        sequenceInitialized_ = true;

        EmitFrame(type, sequence, payload, length);
        partial_.erase(partial_.begin(), partial_.begin() + kHeaderSize + length);
    }
}

void VpcmParser::ResetGeneration()
{
    partial_.clear();
    sequenceInitialized_ = false;
    expectedSequence_ = 0;
    ++generation_;
}

uint64_t VpcmParser::errorCount(VpcmError error) const
{
    switch (error) {
    case VpcmError::BadMagic: return badMagic_;
    case VpcmError::BadVersion: return badVersion_;
    case VpcmError::BadLength: return badLength_;
    case VpcmError::UnknownType: return unknownType_;
    case VpcmError::CrcMismatch: return crcMismatch_;
    case VpcmError::SequenceGap: return sequenceGaps_;
    default: return 0;
    }
}

void VpcmParser::ReportError(VpcmError error)
{
    switch (error) {
    case VpcmError::BadMagic: ++badMagic_; break;
    case VpcmError::BadVersion: ++badVersion_; break;
    case VpcmError::BadLength: ++badLength_; break;
    case VpcmError::UnknownType: ++unknownType_; break;
    case VpcmError::CrcMismatch: ++crcMismatch_; break;
    case VpcmError::SequenceGap: ++sequenceGaps_; break;
    default: break;
    }
    if (onError_) {
        onError_(error);
    }
}

void VpcmParser::EmitFrame(uint8_t type, uint32_t sequence,
    const uint8_t* payload, size_t payloadSize)
{
    VpcmFrame frame;
    frame.generation = generation_;
    frame.type = type;
    frame.sequence = sequence;
    frame.payload.assign(payload, payload + payloadSize);
    ++frameCount_;
    if (onFrame_) {
        onFrame_(frame);
    }
}

} // namespace vaderhaptics
