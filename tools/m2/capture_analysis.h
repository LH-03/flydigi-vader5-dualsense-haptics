#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace vaderhaptics {

// VHTC is a diagnostic-only capture format. Frame payloads remain verbatim
// VPCM payloads; marker and health records add no haptic interpretation.
enum class CaptureRecordKind : uint8_t {
    Frame = 0x46,
    Marker = 0x4D,
    Health = 0x48,
};

inline constexpr uint32_t kCaptureTraceVersionCurrent = 2;
inline constexpr uint32_t kCaptureTraceMaxRecordBytes = 1024 * 1024;

struct CaptureTraceRecord {
    CaptureRecordKind kind = CaptureRecordKind::Frame;
    int64_t timestampUs = 0;
    uint32_t generation = 0;
    uint8_t type = 0;
    uint32_t sequence = 0;
    std::vector<uint8_t> payload;
};

struct CaptureAnalysisOptions {
    int64_t preWindowUs = 250000;
    int64_t postWindowUs = 1000000;
};

struct MarkerAnalysis {
    int64_t timestampUs = 0;
    std::string markerType;
    bool nearestFramePresent = false;
    int64_t nearestFrameDeltaUs = 0;
    uint32_t nearestGeneration = 0;
    uint32_t nearestSequence = 0;
    uint8_t nearestType = 0;
    uint32_t frames81 = 0;
    uint32_t frames83 = 0;
    uint32_t frames84 = 0;
    bool preFeedbackPresent = false;
    bool activity84Present = false;
    uint32_t feedbackChangedBytesMax = 0;
    uint32_t carrierChangedBytesMax = 0;
    uint64_t feedbackAbsDeltaMax = 0;
    uint64_t carrierAbsDeltaMax = 0;
    uint64_t feedbackHashFirst84 = 0;
    uint64_t feedbackHashLast84 = 0;
    uint32_t pcmSampleCount = 0;
    double pcmRms = 0.0;
    int32_t pcmPeak = 0;
    std::vector<uint32_t> generations;
};

struct RepeatabilitySummary {
    std::string markerType;
    uint32_t repetitions = 0;
    uint32_t repetitionsWith84 = 0;
    uint32_t repetitionsWith84Change = 0;
    uint32_t repetitionsWithPcm = 0;
    double meanFrames81 = 0.0;
    double meanFrames83 = 0.0;
    double meanFrames84 = 0.0;
    double meanPcmRms = 0.0;
    double meanFeedbackChangedBytesMax = 0.0;
    double meanCarrierChangedBytesMax = 0.0;
};

struct CaptureAnalysis {
    uint32_t traceVersion = 0;
    CaptureAnalysisOptions options;
    std::vector<MarkerAnalysis> markers;
    std::vector<RepeatabilitySummary> repetitions;
    std::string healthText;
};

// Production VHTC writer entry points shared by runtime capture and tests.
// Records remain bounded diagnostic data and frame payloads stay verbatim.
bool WriteTraceHeader(std::ostream& output,
    uint32_t traceVersion = kCaptureTraceVersionCurrent);

bool AppendTraceRecord(std::ostream& output,
    const CaptureTraceRecord& record);

bool ReadCaptureTrace(const std::string& path,
    std::vector<CaptureTraceRecord>& records, uint32_t& traceVersion,
    std::string& error);

CaptureAnalysis AnalyzeCaptureTrace(const std::vector<CaptureTraceRecord>& records,
    uint32_t traceVersion, const CaptureAnalysisOptions& options = {});

std::string FormatCaptureAnalysis(const CaptureAnalysis& analysis);

} // namespace vaderhaptics
