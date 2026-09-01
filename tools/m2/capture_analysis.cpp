#include "m2/capture_analysis.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace vaderhaptics {
namespace {

constexpr char kTraceMagic[] = {'V', 'H', 'T', 'C'};
constexpr size_t kFeedbackObjectSize = 474;
constexpr size_t kAtomicPayloadSize = 2 + kFeedbackObjectSize + 1920;

bool ReadExact(std::ifstream& file, void* destination, size_t size)
{
    file.read(reinterpret_cast<char*>(destination),
        static_cast<std::streamsize>(size));
    return file.good();
}

bool IsFrame(const CaptureTraceRecord& record)
{
    return record.kind == CaptureRecordKind::Frame;
}

bool IsSupportedRecordKind(CaptureRecordKind kind)
{
    return kind == CaptureRecordKind::Frame ||
        kind == CaptureRecordKind::Marker ||
        kind == CaptureRecordKind::Health;
}

const uint8_t* FeedbackObject(const CaptureTraceRecord& record)
{
    if (record.type == 0x81 || record.type == 0x84) {
        return record.payload.size() == kFeedbackObjectSize
            ? record.payload.data()
            : nullptr;
    }
    if (record.type == 0x83 && record.payload.size() == kAtomicPayloadSize &&
        record.payload[0] == static_cast<uint8_t>(kFeedbackObjectSize & 0xFF) &&
        record.payload[1] == static_cast<uint8_t>(kFeedbackObjectSize >> 8)) {
        return record.payload.data() + 2;
    }
    return nullptr;
}

uint64_t Fnv1a64(const uint8_t* data, size_t size)
{
    uint64_t hash = 14695981039346656037ull;
    for (size_t index = 0; index < size; ++index) {
        hash ^= data[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

uint32_t ChangedBytes(const uint8_t* before, const uint8_t* after,
    size_t begin, size_t end)
{
    uint32_t changed = 0;
    for (size_t index = begin; index < end; ++index) {
        changed += before[index] != after[index] ? 1u : 0u;
    }
    return changed;
}

uint64_t AbsoluteDelta(const uint8_t* before, const uint8_t* after,
    size_t begin, size_t end)
{
    uint64_t total = 0;
    for (size_t index = begin; index < end; ++index) {
        const int delta = static_cast<int>(after[index]) -
            static_cast<int>(before[index]);
        total += static_cast<uint64_t>(delta < 0 ? -delta : delta);
    }
    return total;
}

int16_t Load16LE(const uint8_t* value)
{
    const uint16_t raw = static_cast<uint16_t>(value[0]) |
        (static_cast<uint16_t>(value[1]) << 8);
    return static_cast<int16_t>(raw);
}

void AddGeneration(MarkerAnalysis& marker, uint32_t generation)
{
    if (std::find(marker.generations.begin(), marker.generations.end(), generation) ==
        marker.generations.end()) {
        marker.generations.push_back(generation);
    }
}

} // namespace

bool WriteTraceHeader(std::ostream& output, uint32_t traceVersion)
{
    if (traceVersion != 1 && traceVersion != kCaptureTraceVersionCurrent) {
        return false;
    }
    output.write(kTraceMagic, sizeof(kTraceMagic));
    output.write(reinterpret_cast<const char*>(&traceVersion),
        sizeof(traceVersion));
    return output.good();
}

bool AppendTraceRecord(std::ostream& output,
    const CaptureTraceRecord& record)
{
    if (!IsSupportedRecordKind(record.kind) ||
        record.payload.size() > kCaptureTraceMaxRecordBytes) {
        return false;
    }

    const uint8_t rawKind = static_cast<uint8_t>(record.kind);
    const uint32_t length = static_cast<uint32_t>(record.payload.size());
    output.write(reinterpret_cast<const char*>(&rawKind), sizeof(rawKind));
    output.write(reinterpret_cast<const char*>(&record.timestampUs),
        sizeof(record.timestampUs));
    output.write(reinterpret_cast<const char*>(&record.generation),
        sizeof(record.generation));
    output.write(reinterpret_cast<const char*>(&record.type), sizeof(record.type));
    output.write(reinterpret_cast<const char*>(&record.sequence),
        sizeof(record.sequence));
    output.write(reinterpret_cast<const char*>(&length), sizeof(length));
    if (length > 0) {
        output.write(reinterpret_cast<const char*>(record.payload.data()),
            static_cast<std::streamsize>(length));
    }
    return output.good();
}

bool ReadCaptureTrace(const std::string& path,
    std::vector<CaptureTraceRecord>& records, uint32_t& traceVersion,
    std::string& error)
{
    records.clear();
    traceVersion = 0;
    error.clear();

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open trace";
        return false;
    }

    char magic[4] = {};
    if (!ReadExact(file, magic, sizeof(magic)) ||
        std::memcmp(magic, kTraceMagic, sizeof(magic)) != 0 ||
        !ReadExact(file, &traceVersion, sizeof(traceVersion))) {
        error = "invalid or truncated trace header";
        return false;
    }
    if (traceVersion != 1 && traceVersion != kCaptureTraceVersionCurrent) {
        error = "unsupported trace version " + std::to_string(traceVersion);
        return false;
    }

    while (true) {
        uint8_t rawKind = 0;
        file.read(reinterpret_cast<char*>(&rawKind), sizeof(rawKind));
        if (file.eof()) {
            break;
        }
        if (!file.good()) {
            error = "truncated record kind";
            return false;
        }
        if (rawKind != static_cast<uint8_t>(CaptureRecordKind::Frame) &&
            rawKind != static_cast<uint8_t>(CaptureRecordKind::Marker) &&
            rawKind != static_cast<uint8_t>(CaptureRecordKind::Health)) {
            error = "unknown trace record kind";
            return false;
        }

        CaptureTraceRecord record;
        record.kind = static_cast<CaptureRecordKind>(rawKind);
        uint32_t length = 0;
        if (!ReadExact(file, &record.timestampUs, sizeof(record.timestampUs)) ||
            !ReadExact(file, &record.generation, sizeof(record.generation)) ||
            !ReadExact(file, &record.type, sizeof(record.type)) ||
            !ReadExact(file, &record.sequence, sizeof(record.sequence)) ||
            !ReadExact(file, &length, sizeof(length))) {
            error = "truncated trace record header";
            return false;
        }
        if (length > kCaptureTraceMaxRecordBytes) {
            error = "trace record exceeds diagnostic limit";
            return false;
        }
        record.payload.resize(length);
        if (length > 0 && !ReadExact(file, record.payload.data(), length)) {
            error = "truncated trace record payload";
            return false;
        }
        records.push_back(std::move(record));
    }
    return true;
}

CaptureAnalysis AnalyzeCaptureTrace(const std::vector<CaptureTraceRecord>& records,
    uint32_t traceVersion, const CaptureAnalysisOptions& options)
{
    CaptureAnalysis analysis;
    analysis.traceVersion = traceVersion;
    analysis.options = options;

    std::vector<const CaptureTraceRecord*> frames;
    std::vector<const CaptureTraceRecord*> markers;
    for (const auto& record : records) {
        if (IsFrame(record)) {
            frames.push_back(&record);
        }
        else if (record.kind == CaptureRecordKind::Marker) {
            markers.push_back(&record);
        }
        else if (record.kind == CaptureRecordKind::Health) {
            if (!analysis.healthText.empty()) {
                analysis.healthText += "\n";
            }
            analysis.healthText.append(record.payload.begin(), record.payload.end());
        }
    }
    auto byTimestamp = [](const CaptureTraceRecord* left,
                           const CaptureTraceRecord* right) {
        return left->timestampUs < right->timestampUs;
    };
    std::sort(frames.begin(), frames.end(), byTimestamp);
    std::sort(markers.begin(), markers.end(), byTimestamp);

    for (const CaptureTraceRecord* markerRecord : markers) {
        MarkerAnalysis marker;
        marker.timestampUs = markerRecord->timestampUs;
        marker.markerType.assign(markerRecord->payload.begin(),
            markerRecord->payload.end());
        const int64_t windowStart = marker.timestampUs - options.preWindowUs;
        const int64_t windowEnd = marker.timestampUs + options.postWindowUs;

        const CaptureTraceRecord* preFeedback = nullptr;
        for (const CaptureTraceRecord* frame : frames) {
            if (frame->timestampUs > marker.timestampUs) {
                break;
            }
            if (FeedbackObject(*frame) != nullptr) {
                preFeedback = frame;
            }
        }
        const uint8_t* preObject = preFeedback ? FeedbackObject(*preFeedback) : nullptr;
        marker.preFeedbackPresent = preObject != nullptr;

        for (const CaptureTraceRecord* frame : frames) {
            if (frame->timestampUs >= marker.timestampUs) {
                marker.nearestFramePresent = true;
                marker.nearestFrameDeltaUs = frame->timestampUs - marker.timestampUs;
                marker.nearestGeneration = frame->generation;
                marker.nearestSequence = frame->sequence;
                marker.nearestType = frame->type;
                break;
            }
        }

        long double pcmSquareSum = 0.0;
        for (const CaptureTraceRecord* frame : frames) {
            if (frame->timestampUs < windowStart) {
                continue;
            }
            if (frame->timestampUs > windowEnd) {
                break;
            }
            AddGeneration(marker, frame->generation);
            if (frame->type == 0x81) {
                ++marker.frames81;
            }
            else if (frame->type == 0x83) {
                ++marker.frames83;
                if (frame->payload.size() == kAtomicPayloadSize &&
                    FeedbackObject(*frame) != nullptr) {
                    const uint8_t* pcm = frame->payload.data() + 2 + kFeedbackObjectSize;
                    for (size_t index = 0; index < 1920; index += 2) {
                        const int32_t sample = Load16LE(pcm + index);
                        const int32_t magnitude = sample == std::numeric_limits<int16_t>::min()
                            ? 32768
                            : std::abs(sample);
                        pcmSquareSum += static_cast<long double>(sample) * sample;
                        marker.pcmPeak = std::max(marker.pcmPeak, magnitude);
                        ++marker.pcmSampleCount;
                    }
                }
            }
            else if (frame->type == 0x84) {
                ++marker.frames84;
                const uint8_t* object = FeedbackObject(*frame);
                if (object == nullptr) {
                    continue;
                }
                marker.activity84Present = true;
                const uint64_t hash = Fnv1a64(object, kFeedbackObjectSize);
                if (marker.feedbackHashFirst84 == 0) {
                    marker.feedbackHashFirst84 = hash;
                }
                marker.feedbackHashLast84 = hash;
                if (preObject != nullptr) {
                    marker.feedbackChangedBytesMax = std::max(
                        marker.feedbackChangedBytesMax,
                        ChangedBytes(preObject, object, 0, 76));
                    marker.carrierChangedBytesMax = std::max(
                        marker.carrierChangedBytesMax,
                        ChangedBytes(preObject, object, 76, kFeedbackObjectSize));
                    marker.feedbackAbsDeltaMax = std::max(marker.feedbackAbsDeltaMax,
                        AbsoluteDelta(preObject, object, 0, 76));
                    marker.carrierAbsDeltaMax = std::max(marker.carrierAbsDeltaMax,
                        AbsoluteDelta(preObject, object, 76, kFeedbackObjectSize));
                }
            }
        }
        if (marker.pcmSampleCount > 0) {
            marker.pcmRms = std::sqrt(static_cast<double>(pcmSquareSum /
                static_cast<long double>(marker.pcmSampleCount)));
        }
        analysis.markers.push_back(std::move(marker));
    }

    for (const MarkerAnalysis& marker : analysis.markers) {
        auto existing = std::find_if(analysis.repetitions.begin(),
            analysis.repetitions.end(), [&](const RepeatabilitySummary& summary) {
                return summary.markerType == marker.markerType;
            });
        if (existing == analysis.repetitions.end()) {
            RepeatabilitySummary summary;
            summary.markerType = marker.markerType;
            analysis.repetitions.push_back(std::move(summary));
            existing = std::prev(analysis.repetitions.end());
        }
        RepeatabilitySummary& summary = *existing;
        ++summary.repetitions;
        summary.repetitionsWith84 += marker.activity84Present ? 1u : 0u;
        summary.repetitionsWith84Change +=
            marker.feedbackChangedBytesMax > 0 || marker.carrierChangedBytesMax > 0 ? 1u : 0u;
        summary.repetitionsWithPcm += marker.pcmSampleCount > 0 ? 1u : 0u;
        summary.meanFrames81 += marker.frames81;
        summary.meanFrames83 += marker.frames83;
        summary.meanFrames84 += marker.frames84;
        summary.meanPcmRms += marker.pcmRms;
        summary.meanFeedbackChangedBytesMax += marker.feedbackChangedBytesMax;
        summary.meanCarrierChangedBytesMax += marker.carrierChangedBytesMax;
    }
    for (RepeatabilitySummary& summary : analysis.repetitions) {
        const double count = static_cast<double>(summary.repetitions);
        summary.meanFrames81 /= count;
        summary.meanFrames83 /= count;
        summary.meanFrames84 /= count;
        summary.meanPcmRms /= count;
        summary.meanFeedbackChangedBytesMax /= count;
        summary.meanCarrierChangedBytesMax /= count;
    }
    return analysis;
}

std::string FormatCaptureAnalysis(const CaptureAnalysis& analysis)
{
    std::ostringstream output;
    output << std::fixed << std::setprecision(3);
    output << "trace_version: " << analysis.traceVersion << "\n";
    output << "analysis_window_us: -" << analysis.options.preWindowUs
           << " .. +" << analysis.options.postWindowUs << "\n";
    output << "marker_count: " << analysis.markers.size() << "\n";
    for (const MarkerAnalysis& marker : analysis.markers) {
        output << "marker timestamp_us=" << marker.timestampUs
               << " type=" << marker.markerType;
        if (marker.nearestFramePresent) {
            output << " nearest_vpcm_generation=" << marker.nearestGeneration
                   << " sequence=" << marker.nearestSequence
                   << " type=0x" << std::hex << static_cast<unsigned>(marker.nearestType)
                   << std::dec << " delta_us=" << marker.nearestFrameDeltaUs;
        }
        else {
            output << " nearest_vpcm=none";
        }
        output << "\n";
        output << "  window_frames 0x81=" << marker.frames81
               << " 0x83=" << marker.frames83 << " 0x84=" << marker.frames84
               << " generations=";
        if (marker.generations.empty()) {
            output << "none";
        }
        else {
            for (size_t index = 0; index < marker.generations.size(); ++index) {
                if (index != 0) output << ',';
                output << marker.generations[index];
            }
        }
        output << "\n";
        output << "  realtime_0x84 pre_feedback="
               << (marker.preFeedbackPresent ? "yes" : "no")
               << " active=" << (marker.activity84Present ? "yes" : "no")
               << " feedback_changed_bytes_max=" << marker.feedbackChangedBytesMax
               << " carrier_changed_bytes_max=" << marker.carrierChangedBytesMax
               << " feedback_abs_delta_max=" << marker.feedbackAbsDeltaMax
               << " carrier_abs_delta_max=" << marker.carrierAbsDeltaMax
               << " feedback_hash_first=0x" << std::hex << marker.feedbackHashFirst84
               << " feedback_hash_last=0x" << marker.feedbackHashLast84 << std::dec << "\n";
        output << "  pcm_0x83 samples=" << marker.pcmSampleCount
               << " rms=" << marker.pcmRms << " peak=" << marker.pcmPeak << "\n";
    }
    for (const RepeatabilitySummary& summary : analysis.repetitions) {
        output << "repeatability marker_type=" << summary.markerType
               << " repetitions=" << summary.repetitions
               << " with_0x84=" << summary.repetitionsWith84
               << " with_0x84_change=" << summary.repetitionsWith84Change
               << " with_pcm=" << summary.repetitionsWithPcm
               << " mean_frames_0x81=" << summary.meanFrames81
               << " mean_frames_0x83=" << summary.meanFrames83
               << " mean_frames_0x84=" << summary.meanFrames84
               << " mean_pcm_rms=" << summary.meanPcmRms
               << " mean_feedback_changed_bytes_max=" << summary.meanFeedbackChangedBytesMax
               << " mean_carrier_changed_bytes_max=" << summary.meanCarrierChangedBytesMax
               << "\n";
    }
    output << "capture_health:\n";
    if (analysis.healthText.empty()) {
        output << "unavailable (legacy trace without final health record)\n";
    }
    else {
        output << analysis.healthText;
        if (analysis.healthText.back() != '\n') output << '\n';
    }
    output << "note: byte changes, hashes, and PCM energy are correlation diagnostics; "
           << "they do not establish haptic semantics.\n";
    return output.str();
}

} // namespace vaderhaptics
