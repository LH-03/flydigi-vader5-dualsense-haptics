#include "m3/haptics.h"

#include "m2/capture_analysis.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace vaderhaptics {
namespace m3 {
namespace {

constexpr size_t kFeedbackObjectSize = 474;
constexpr size_t kHapticsRegionOffset = 154;
constexpr size_t kHapticsRegionSize = 64;
constexpr size_t kRightTriggerOffset = 6;
constexpr size_t kLeftTriggerOffset = 17;
constexpr int kSamplesPerBlock = 32;
constexpr double kBlockSeconds = 32.0 / 3000.0;

double Percentile(std::vector<uint64_t> values, double quantile)
{
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const size_t index = static_cast<size_t>(
        quantile * static_cast<double>(values.size() - 1));
    return static_cast<double>(values[index]);
}

int64_t NowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool OverlappedWrite(void* handle, const uint8_t* data, DWORD size,
    DWORD& error)
{
    error = ERROR_SUCCESS;
    if (handle == nullptr) {
        error = ERROR_INVALID_HANDLE;
        return false;
    }
    OVERLAPPED overlapped = {};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) {
        error = GetLastError();
        return false;
    }

    const BOOL completedImmediately =
        WriteFile(handle, data, size, nullptr, &overlapped);
    if (!completedImmediately) {
        error = GetLastError();
        if (error != ERROR_IO_PENDING) {
            CloseHandle(overlapped.hEvent);
            return false;
        }
        const DWORD wait = WaitForSingleObject(overlapped.hEvent, 100);
        if (wait != WAIT_OBJECT_0) {
            CancelIoEx(handle, &overlapped);
            WaitForSingleObject(overlapped.hEvent, INFINITE);
            DWORD ignored = 0;
            GetOverlappedResult(handle, &overlapped, &ignored, FALSE);
            error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
            CloseHandle(overlapped.hEvent);
            return false;
        }
    }

    DWORD transferred = 0;
    const BOOL completed = GetOverlappedResult(
        handle, &overlapped, &transferred, FALSE);
    const bool ok = completed != 0 && transferred == size;
    if (!completed) {
        error = GetLastError();
    }
    else if (transferred != size) {
        error = ERROR_WRITE_FAULT;
    }
    CloseHandle(overlapped.hEvent);
    return ok;
}

int16_t Load16LE(const uint8_t* value)
{
    const uint16_t raw = static_cast<uint16_t>(value[0]) |
        (static_cast<uint16_t>(value[1]) << 8);
    return static_cast<int16_t>(raw);
}

DualSenseTriggerEffect DecodeTriggerEffect(const uint8_t* feedback,
    size_t offset)
{
    DualSenseTriggerEffect effect;
    effect.mode = feedback[offset];
    effect.startResistance = feedback[offset + 1];
    effect.effectForce = feedback[offset + 2];
    effect.rangeForce = feedback[offset + 3];
    effect.nearReleaseStrength = feedback[offset + 4];
    effect.nearMiddleStrength = feedback[offset + 5];
    effect.pressedStrength = feedback[offset + 6];
    effect.frequency = feedback[offset + 9];
    return effect;
}

} // namespace

bool DecodeDualSenseHapticsBlock(const uint8_t* feedback474, size_t size,
    DualSenseHapticsBlock& out)
{
    out = DualSenseHapticsBlock{};
    if (feedback474 == nullptr || size != kFeedbackObjectSize) {
        return false;
    }
    for (int index = 0; index < kSamplesPerBlock; ++index) {
        const size_t offset = kHapticsRegionOffset + static_cast<size_t>(index) * 2;
        out.samples[index].left =
            static_cast<int8_t>(feedback474[offset]);
        out.samples[index].right =
            static_cast<int8_t>(feedback474[offset + 1]);
    }
    out.rightTrigger = DecodeTriggerEffect(feedback474, kRightTriggerOffset);
    out.leftTrigger = DecodeTriggerEffect(feedback474, kLeftTriggerOffset);
    return true;
}

void LimitVaderHapticFrame(VaderHapticFrame& frame, double limiterCap)
{
    const float cap = static_cast<float>(
        std::clamp(limiterCap, 0.0, 128.0 / 255.0));
    frame.leftGrip = std::clamp(frame.leftGrip, 0.0f, cap);
    frame.rightGrip = std::clamp(frame.rightGrip, 0.0f, cap);
    frame.leftTrigger = std::clamp(frame.leftTrigger, 0.0f, cap);
    frame.rightTrigger = std::clamp(frame.rightTrigger, 0.0f, cap);
}

bool LoadCalibrationConfig(const std::string& path, RendererConfig& config,
    std::string& error)
{
    // Conservative fallback baseline: no minimum-drive compensation, initial
    // physical cap clamped to 128/255.
    config.minDrive = 0.0;
    config.limiterCap = std::min(config.limiterCap, 128.0 / 255.0);

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "calibration config not found: " + path;
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());

    auto extractNumber = [&](const char* key) -> double {
        const std::string needle = std::string("\"") + key + "\"";
        const size_t position = text.find(needle);
        if (position == std::string::npos) {
            return -1.0;
        }
        const size_t colon = text.find(':', position + needle.size());
        if (colon == std::string::npos) {
            return -1.0;
        }
        size_t start = colon + 1;
        while (start < text.size() &&
               (text[start] == ' ' || text[start] == '\t' ||
                   text[start] == '\r' || text[start] == '\n')) {
            ++start;
        }
        size_t end = start;
        while (end < text.size() && text[end] != ',' && text[end] != '}' &&
               text[end] != '\r' && text[end] != '\n') {
            ++end;
        }
        const std::string value = text.substr(start, end - start);
        if (value.empty()) {
            return -1.0;
        }
        try {
            return std::stod(value);
        }
        catch (...) {
            return -1.0;
        }
    };

    double recommendedMin = extractNumber("min_drive_amplitude");
    if (recommendedMin < 0.0) {
        recommendedMin = extractNumber("recommended_min_amplitude");
    }
    if (recommendedMin < 0.0 || recommendedMin > 255.0) {
        error = "calibration config missing/invalid "
                "recommended_min_amplitude; using safe fallback";
        return false;
    }
    const double requestedCap = extractNumber("limiter_cap_amplitude");
    if (requestedCap >= 0.0) {
        if (requestedCap > 128.0 || requestedCap < recommendedMin) {
            error = "calibration config invalid limiter_cap_amplitude; "
                    "using safe fallback";
            config.minDrive = 0.0;
            return false;
        }
        config.limiterCap = requestedCap / 255.0;
    }
    const double sourceGain = extractNumber("source_gain");
    if (sourceGain >= 0.0) {
        if (sourceGain < 0.1 || sourceGain > 8.0) {
            error = "calibration config invalid source_gain; using safe "
                    "fallback";
            config.minDrive = 0.0;
            return false;
        }
        config.sourceGain = sourceGain;
    }
    const double responseGamma = extractNumber("response_gamma");
    if (responseGamma >= 0.0) {
        if (responseGamma < 0.25 || responseGamma > 4.0) {
            error = "calibration config invalid response_gamma; using safe "
                    "fallback";
            config.minDrive = 0.0;
            return false;
        }
        config.gamma = responseGamma;
    }
    const double fidelityModel = extractNumber("fidelity_model");
    if (fidelityModel >= 0.0) {
        if (fidelityModel == 1.0) {
            config.fidelityModel = FidelityModel::FirstPlayableV1;
        }
        else if (fidelityModel == 2.0) {
            config.fidelityModel = FidelityModel::TemporalContrastV2;
        }
        else {
            error = "calibration config invalid fidelity_model; using safe "
                    "fallback";
            config.minDrive = 0.0;
            return false;
        }
    }
    const double transientBoost = extractNumber("transient_boost");
    if (transientBoost >= 0.0) {
        if (transientBoost > 1.0) {
            error = "calibration config invalid transient_boost; using safe "
                    "fallback";
            config.minDrive = 0.0;
            return false;
        }
        config.transientBoost = transientBoost;
    }
    auto loadOptional = [&](const char* key, double minimum, double maximum,
                            double& target) -> bool {
        const double value = extractNumber(key);
        if (value < 0.0) {
            return true;
        }
        if (value < minimum || value > maximum) {
            error = std::string("calibration config invalid ") + key +
                "; using safe fallback";
            config.minDrive = 0.0;
            return false;
        }
        target = value;
        return true;
    };
    if (!loadOptional("texture_floor", 0.0, 1.0,
            config.textureFloor) ||
        !loadOptional("texture_roughness_gain", 0.0, 1.0,
            config.textureRoughnessGain) ||
        !loadOptional("roughness_reference", 0.005, 0.5,
            config.roughnessReference) ||
        !loadOptional("relative_motion_reference", 0.05, 1.5,
            config.relativeMotionReference) ||
        !loadOptional("slow_context_seconds", 0.04, 0.5,
            config.slowContextS) ||
        !loadOptional("novelty_mix", 0.0, 1.5,
            config.noveltyMix) ||
        !loadOptional("impact_gain", 0.0, 1.0,
            config.impactGain) ||
        !loadOptional("impact_threshold", 0.0, 0.5,
            config.impactThreshold) ||
        !loadOptional("impact_full_scale", 0.01, 1.0,
            config.impactFullScale) ||
        !loadOptional("impact_release_seconds", 0.005, 0.25,
            config.impactReleaseS) ||
        !loadOptional("trigger_fallback_gain", 0.0, 128.0 / 255.0,
            config.triggerFallbackGain) ||
        !loadOptional("trigger_fallback_onset_gate", 0.01, 0.8,
            config.triggerFallbackOnsetGate) ||
        !loadOptional("trigger_fallback_pulse_seconds", 0.01, 0.15,
            config.triggerFallbackPulseS) ||
        !loadOptional("trigger_fallback_refractory_seconds", 0.02, 0.5,
            config.triggerFallbackRefractoryS) ||
        !loadOptional("trigger_fallback_gamma", 0.35, 1.5,
            config.triggerFallbackGamma)) {
        return false;
    }
    double triggerCapAmplitude = config.triggerFallbackCap * 255.0;
    if (!loadOptional("trigger_fallback_cap_amplitude", 0.0, 128.0,
            triggerCapAmplitude)) {
        return false;
    }
    config.triggerFallbackCap = triggerCapAmplitude / 255.0;
    double triggerDeadzone = config.triggerFallbackDeadzone;
    if (!loadOptional("trigger_fallback_deadzone", 0.0, 224.0,
            triggerDeadzone)) {
        return false;
    }
    config.triggerFallbackDeadzone = static_cast<uint8_t>(triggerDeadzone);
    if (config.impactFullScale <= config.impactThreshold) {
        error = "calibration config impact_full_scale must exceed "
                "impact_threshold; using safe fallback";
        config.minDrive = 0.0;
        return false;
    }
    if (config.triggerFallbackRefractoryS < config.triggerFallbackPulseS) {
        error = "calibration config trigger fallback refractory period must "
                "not be shorter than pulse; using safe fallback";
        config.minDrive = 0.0;
        return false;
    }
    config.minDrive = recommendedMin / 255.0;
    config.limiterCap = std::min(config.limiterCap, 128.0 / 255.0);
    error.clear();
    return true;
}

HapticRenderer::HapticRenderer(const RendererConfig& config)
    : config_(config)
{
    attackCoeff_ = 1.0 - std::exp(-kBlockSeconds / std::max(config_.attackS, 1e-6));
    releaseCoeff_ = 1.0 - std::exp(-kBlockSeconds / std::max(config_.releaseS, 1e-6));
    impactReleaseCoeff_ = 1.0 - std::exp(
        -kBlockSeconds / std::max(config_.impactReleaseS, 1e-6));
    slowContextCoeff_ = 1.0 - std::exp(
        -kBlockSeconds / std::max(config_.slowContextS, 1e-6));
    alphaLow_ = 1.0 - std::exp(
        -2.0 * 3.14159265358979323846 * std::max(config_.lowCutoffHz, 1.0) /
        3000.0);
}

void HapticRenderer::Reset()
{
    envLeft_ = 0.0;
    envRight_ = 0.0;
    transientLeft_ = 0.0;
    transientRight_ = 0.0;
    previousEnergyLeft_ = 0.0;
    previousEnergyRight_ = 0.0;
    slowContextLeft_ = 0.0;
    slowContextRight_ = 0.0;
    monoLow_ = 0.0;
    previousLeftSample_ = 0;
    previousRightSample_ = 0;
    previousHighSample_ = 0.0;
    envLow_ = 0.0;
    envHigh_ = 0.0;
    transientLow_ = 0.0;
    transientMono_ = 0.0;
    previousEnergyLow_ = 0.0;
    previousEnergyHigh_ = 0.0;
    slowContextLow_ = 0.0;
    slowContextHigh_ = 0.0;
    lastDrive_ = VaderHapticFrame{};
    lastDrive_.timestampUs = lastSourceUs_;
}

double HapticRenderer::ProcessChannel(const ChannelSignal& signal,
    double& envState, double& transientState, double& previousEnergy,
    double& slowContext)
{
    if (signal.energy <= 0.0) {
        envState = 0.0;
        transientState = 0.0;
        previousEnergy = 0.0;
        slowContext = 0.0;
        return 0.0;
    }
    const double immediateOnset = std::max(
        0.0, signal.energy - previousEnergy);
    double onset = immediateOnset;
    if (config_.fidelityModel == FidelityModel::TemporalContrastV2) {
        const double contextualNovelty = std::max(
            0.0, signal.energy - slowContext);
        const double sparseGate = signal.crest * signal.crest;
        onset = std::max(onset,
            config_.noveltyMix * contextualNovelty * sparseGate);
        slowContext +=
            (signal.energy - slowContext) * slowContextCoeff_;
    }
    else {
        slowContext = signal.energy;
    }
    previousEnergy = signal.energy;
    if (signal.energy > envState) {
        envState += (signal.energy - envState) * attackCoeff_;
    }
    else {
        envState += (signal.energy - envState) * releaseCoeff_;
    }

    const double impactLinear = std::max(0.0,
        (onset - config_.impactThreshold) /
            (config_.impactFullScale - config_.impactThreshold));
    double impactTarget = impactLinear / (1.0 + impactLinear);
    if (config_.fidelityModel == FidelityModel::TemporalContrastV2) {
        impactTarget *= 0.22 + 0.78 * signal.crest;
        impactTarget = std::clamp(impactTarget *
            (1.0 + config_.transientBoost * signal.crest), 0.0, 1.0);
    }
    else {
        impactTarget *= 0.55 + 0.45 * signal.crest;
        impactTarget = std::clamp(impactTarget *
            (1.0 + config_.transientBoost * signal.roughness), 0.0, 1.0);
    }
    if (impactTarget > transientState) {
        transientState = impactTarget;
    }
    else {
        transientState +=
            (impactTarget - transientState) * impactReleaseCoeff_;
    }

    double value = envState * config_.sourceGain;
    value += config_.liftStrength * config_.weakLift *
        std::exp(-value / (config_.weakLift + 1e-9));
    value = std::pow(std::max(value, 0.0), config_.gamma);
    value *= 1.0 - config_.softKnee * value * (1.0 - value);
    const double textureShape = std::clamp(config_.textureFloor +
        config_.textureRoughnessGain * signal.roughness, 0.0, 1.0);
    const double texture = value * textureShape;
    const double impactExponent =
        config_.fidelityModel == FidelityModel::TemporalContrastV2
        ? 0.64 : 0.72;
    const double impact = config_.impactGain *
        std::pow(std::max(transientState, 0.0), impactExponent);
    value = std::clamp(std::max(texture, impact), 0.0, 1.0);

    // Preserve the source's full 0..1 intensity range inside the calibrated
    // physical window instead of hard-clipping most menu levels at the cap.
    const double floor = std::min(config_.minDrive, config_.limiterCap);
    return floor + (config_.limiterCap - floor) * value;
}

void HapticRenderer::ProcessBlockPolicies(const DualSenseHapticsBlock& block,
    ChannelSignal& leftSignal, ChannelSignal& rightSignal)
{
    if (config_.policy == RendererPolicy::StereoEnvelope) {
        double peakL = 0.0, peakR = 0.0, meanL = 0.0, meanR = 0.0;
        double squareL = 0.0, squareR = 0.0;
        double deltaL = 0.0, deltaR = 0.0;
        int signChangesL = 0, signChangesR = 0;
        int previousL = previousLeftSample_;
        int previousR = previousRightSample_;
        for (int index = 0; index < kSamplesPerBlock; ++index) {
            const int left = block.samples[index].left;
            const int right = block.samples[index].right;
            peakL = std::max(peakL, static_cast<double>(std::abs(left)));
            peakR = std::max(peakR, static_cast<double>(std::abs(right)));
            meanL += static_cast<double>(std::abs(left));
            meanR += static_cast<double>(std::abs(right));
            squareL += static_cast<double>(left * left);
            squareR += static_cast<double>(right * right);
            deltaL += static_cast<double>(std::abs(left - previousL));
            deltaR += static_cast<double>(std::abs(right - previousR));
            if (left != 0 && previousL != 0 &&
                (left < 0) != (previousL < 0)) {
                ++signChangesL;
            }
            if (right != 0 && previousR != 0 &&
                (right < 0) != (previousR < 0)) {
                ++signChangesR;
            }
            previousL = left;
            previousR = right;
        }
        previousLeftSample_ = previousL;
        previousRightSample_ = previousR;
        const double count = static_cast<double>(kSamplesPerBlock);
        const double meanAbsL = meanL / count / 127.0;
        const double meanAbsR = meanR / count / 127.0;
        const double normalizedPeakL = peakL / 127.0;
        const double normalizedPeakR = peakR / 127.0;
        const double rmsL = std::sqrt(squareL / count) / 127.0;
        const double rmsR = std::sqrt(squareR / count) / 127.0;
        leftSignal.energy = 0.35 * normalizedPeakL + 0.65 * rmsL;
        rightSignal.energy = 0.35 * normalizedPeakR + 0.65 * rmsR;
        if (config_.fidelityModel == FidelityModel::TemporalContrastV2) {
            const double motionL = (deltaL / count / 127.0) /
                std::max(rmsL, 1.0 / 127.0);
            const double motionR = (deltaR / count / 127.0) /
                std::max(rmsR, 1.0 / 127.0);
            const double densityL = static_cast<double>(signChangesL) /
                static_cast<double>(kSamplesPerBlock - 1);
            const double densityR = static_cast<double>(signChangesR) /
                static_cast<double>(kSamplesPerBlock - 1);
            leftSignal.roughness = std::clamp(
                0.70 * motionL /
                    std::max(config_.relativeMotionReference, 1e-6) +
                0.30 * densityL, 0.0, 1.0);
            rightSignal.roughness = std::clamp(
                0.70 * motionR /
                    std::max(config_.relativeMotionReference, 1e-6) +
                0.30 * densityR, 0.0, 1.0);
        }
        else {
            leftSignal.roughness = std::clamp(
                (deltaL / count / 127.0) /
                    std::max(config_.roughnessReference, 1e-6),
                0.0, 1.0);
            rightSignal.roughness = std::clamp(
                (deltaR / count / 127.0) /
                    std::max(config_.roughnessReference, 1e-6),
                0.0, 1.0);
        }
        leftSignal.crest = normalizedPeakL > 0.0
            ? std::clamp((normalizedPeakL - meanAbsL) /
                normalizedPeakL, 0.0, 1.0)
            : 0.0;
        rightSignal.crest = normalizedPeakR > 0.0
            ? std::clamp((normalizedPeakR - meanAbsR) /
                normalizedPeakR, 0.0, 1.0)
            : 0.0;
    }
    else {
        double blockLow = 0.0, blockHigh = 0.0;
        double highDelta = 0.0;
        double previousHigh = previousHighSample_;
        for (int index = 0; index < kSamplesPerBlock; ++index) {
            const double mono =
                (std::abs(block.samples[index].left) +
                    std::abs(block.samples[index].right)) /
                (2.0 * 127.0);
            monoLow_ += alphaLow_ * (mono - monoLow_);
            const double high = mono - monoLow_;
            blockLow = std::max(blockLow, monoLow_);
            blockHigh = std::max(blockHigh, high);
            highDelta += std::fabs(high - previousHigh);
            previousHigh = high;
        }
        previousHighSample_ = previousHigh;
        leftSignal.energy = blockLow;
        rightSignal.energy = blockHigh;
        rightSignal.roughness = std::clamp(
            (highDelta / kSamplesPerBlock) /
                std::max(config_.roughnessReference, 1e-6),
            0.0, 1.0);
        rightSignal.crest = rightSignal.roughness;
    }
}

VaderHapticFrame HapticRenderer::Process(const DualSenseHapticsBlock& block)
{
    lastSourceUs_ = block.timestampUs;
    bool allZero = true;
    for (const auto& sample : block.samples) {
        if (sample.left != 0 || sample.right != 0) {
            allZero = false;
            break;
        }
    }
    VaderHapticFrame drive;
    drive.timestampUs = block.timestampUs;
    if (allZero) {
        envLeft_ = 0.0;
        envRight_ = 0.0;
        transientLeft_ = 0.0;
        transientRight_ = 0.0;
        previousEnergyLeft_ = 0.0;
        previousEnergyRight_ = 0.0;
        slowContextLeft_ = 0.0;
        slowContextRight_ = 0.0;
        monoLow_ = 0.0;
        previousLeftSample_ = 0;
        previousRightSample_ = 0;
        previousHighSample_ = 0.0;
        envLow_ = 0.0;
        envHigh_ = 0.0;
        transientLow_ = 0.0;
        transientMono_ = 0.0;
        previousEnergyLow_ = 0.0;
        previousEnergyHigh_ = 0.0;
        slowContextLow_ = 0.0;
        slowContextHigh_ = 0.0;
        lastDrive_ = drive;
        return drive;
    }

    ChannelSignal leftSignal;
    ChannelSignal rightSignal;
    ProcessBlockPolicies(block, leftSignal, rightSignal);
    if (config_.policy == RendererPolicy::StereoEnvelope) {
        drive.leftGrip = static_cast<float>(
            ProcessChannel(leftSignal, envLeft_, transientLeft_,
                previousEnergyLeft_, slowContextLeft_));
        drive.rightGrip = static_cast<float>(
            ProcessChannel(rightSignal, envRight_, transientRight_,
                previousEnergyRight_, slowContextRight_));
    }
    else {
        drive.leftGrip = static_cast<float>(
            ProcessChannel(leftSignal, envLow_, transientLow_,
                previousEnergyLow_, slowContextLow_));
        drive.rightGrip = static_cast<float>(
            ProcessChannel(rightSignal, envHigh_, transientMono_,
                previousEnergyHigh_, slowContextHigh_));
    }
    lastDrive_ = drive;
    return drive;
}

VaderHapticFrame HapticRenderer::WatchdogZero(int64_t nowUs, int64_t maxAgeUs,
    bool& triggered)
{
    triggered = false;
    if (lastSourceUs_ != 0 && nowUs - lastSourceUs_ > maxAgeUs) {
        triggered = true;
        lastDrive_ = VaderHapticFrame{};
        lastDrive_.timestampUs = nowUs;
    }
    return lastDrive_;
}

TriggerEffectClass ClassifyTriggerEffect(uint8_t mode)
{
    switch (mode) {
    case 0:
    case 5:
        return TriggerEffectClass::Off;
    case 1:
        return TriggerEffectClass::Continuous;
    case 2:
        return TriggerEffectClass::Breakpoint;
    case 6:
    case 37:
    case 38:
        return TriggerEffectClass::Oscillation;
    case 33:
        return TriggerEffectClass::Weapon;
    default:
        return TriggerEffectClass::Unsupported;
    }
}

void TriggerSemanticAdapter::Reset()
{
    left_ = SideState{};
    right_ = SideState{};
}

TriggerSemanticAdapter::TriggerSemanticAdapter(const RendererConfig& config)
    : config_(config)
{
    fallbackReleaseCoeff_ = 1.0 - std::exp(
        -kBlockSeconds / std::max(config_.impactReleaseS, 1e-6));
}

float TriggerSemanticAdapter::ProcessSide(
    const DualSenseTriggerEffect& effect, uint8_t position, int64_t nowUs,
    double sourceEnergy, double sourceCrest, SideState& state)
{
    const TriggerEffectClass family = ClassifyTriggerEffect(effect.mode);
    const bool changed = !(effect == state.lastEffect);
    const uint8_t threshold = static_cast<uint8_t>(
        std::clamp<int>(effect.startResistance, 8, 224));
    const bool crossed = state.lastPosition < threshold && position >= threshold;
    const double pull = static_cast<double>(position) / 255.0;
    const double strength = static_cast<double>(std::max({effect.effectForce,
        effect.rangeForce, effect.nearReleaseStrength,
        effect.nearMiddleStrength, effect.pressedStrength})) / 255.0;

    float output = 0.0f;
    switch (family) {
    case TriggerEffectClass::Off:
        state.pulseUntilUs = 0;
        {
            const double onset = std::max(
                0.0, sourceEnergy - state.previousSourceEnergy);
            state.previousSourceEnergy = sourceEnergy;
            const double linear = std::max(0.0,
                (onset - config_.impactThreshold) /
                    (config_.impactFullScale - config_.impactThreshold));
            double target = linear / (1.0 + linear);
            target *= 0.55 + 0.45 * sourceCrest;
            bool startedFallbackPulse = false;
            if (target >= config_.triggerFallbackOnsetGate &&
                nowUs >= state.fallbackRearmUs) {
                state.fallbackPulse = target;
                startedFallbackPulse = true;
                if (config_.fidelityModel ==
                    FidelityModel::TemporalContrastV2) {
                    state.fallbackPulseStartUs = nowUs;
                }
                state.fallbackPulseUntilUs = nowUs +
                    static_cast<int64_t>(
                        config_.triggerFallbackPulseS * 1000000.0);
                state.fallbackRearmUs = nowUs +
                    static_cast<int64_t>(
                        config_.triggerFallbackRefractoryS * 1000000.0);
            }
            if (config_.fidelityModel == FidelityModel::FirstPlayableV1) {
                if (!startedFallbackPulse &&
                    nowUs < state.fallbackPulseUntilUs) {
                    state.fallbackPulse +=
                        (0.0 - state.fallbackPulse) *
                            fallbackReleaseCoeff_;
                }
                else if (nowUs >= state.fallbackPulseUntilUs) {
                    state.fallbackPulse = 0.0;
                }
            }
            else if (nowUs >= state.fallbackPulseUntilUs) {
                state.fallbackPulse = 0.0;
                state.fallbackPulseStartUs = 0;
            }
            if (position >= config_.triggerFallbackDeadzone &&
                nowUs < state.fallbackPulseUntilUs &&
                state.fallbackPulse > 0.0) {
                const double gate = static_cast<double>(
                    position - config_.triggerFallbackDeadzone) /
                    std::max(1.0, 255.0 -
                        config_.triggerFallbackDeadzone);
                double shapedStrength = state.fallbackPulse;
                double frontLoadedEnvelope = 1.0;
                if (config_.fidelityModel ==
                    FidelityModel::TemporalContrastV2) {
                    const double durationUs = std::max(1.0,
                        config_.triggerFallbackPulseS * 1000000.0);
                    const double phase = std::clamp(
                        static_cast<double>(nowUs -
                            state.fallbackPulseStartUs) / durationUs,
                        0.0, 1.0);
                    frontLoadedEnvelope = std::exp(-3.0 * phase);
                    shapedStrength = std::pow(state.fallbackPulse,
                        config_.triggerFallbackGamma);
                }
                output = static_cast<float>(std::min(
                    config_.triggerFallbackCap,
                    config_.triggerFallbackGain *
                        shapedStrength * frontLoadedEnvelope *
                        std::sqrt(gate)));
            }
        }
        break;
    case TriggerEffectClass::Unsupported:
        state.pulseUntilUs = 0;
        state.previousSourceEnergy = sourceEnergy;
        state.fallbackPulse = 0.0;
        state.fallbackPulseStartUs = 0;
        state.fallbackPulseUntilUs = 0;
        state.fallbackRearmUs = 0;
        break;
    case TriggerEffectClass::Continuous:
        if (position >= threshold) {
            output = static_cast<float>((0.12 + 0.20 * strength) * pull);
        }
        break;
    case TriggerEffectClass::Breakpoint:
    case TriggerEffectClass::Weapon:
        if (crossed || (changed && position >= threshold)) {
            state.pulseUntilUs = nowUs +
                (family == TriggerEffectClass::Weapon ? 65000 : 45000);
        }
        if (position >= threshold && nowUs < state.pulseUntilUs) {
            output = static_cast<float>(0.30 + 0.18 * strength);
        }
        break;
    case TriggerEffectClass::Oscillation:
        if (position >= threshold) {
            const double frequencyHz = 8.0 +
                22.0 * static_cast<double>(effect.frequency) / 255.0;
            const int64_t periodUs = static_cast<int64_t>(1000000.0 / frequencyHz);
            const bool on = periodUs > 0 && (nowUs % periodUs) < periodUs / 2;
            if (on) {
                output = static_cast<float>(0.18 + 0.27 * strength);
            }
        }
        break;
    }

    state.lastEffect = effect;
    state.lastPosition = position;
    return std::min(output, static_cast<float>(config_.limiterCap));
}

void TriggerSemanticAdapter::Process(const DualSenseHapticsBlock& block,
    uint8_t leftPosition, uint8_t rightPosition, VaderHapticFrame& frame)
{
    auto signal = [&](bool left) {
        double peak = 0.0;
        double mean = 0.0;
        double squares = 0.0;
        for (const auto& sample : block.samples) {
            const int value = left ? sample.left : sample.right;
            const double normalized =
                static_cast<double>(value) / 127.0;
            peak = std::max(peak, std::abs(normalized));
            mean += std::abs(normalized);
            squares += normalized * normalized;
        }
        mean /= kSamplesPerBlock;
        const double rms = std::sqrt(squares / kSamplesPerBlock);
        const double energy = 0.35 * peak + 0.65 * rms;
        const double crest = peak > 0.0
            ? std::clamp((peak - mean) / peak, 0.0, 1.0)
            : 0.0;
        return std::pair<double, double>{energy, crest};
    };
    const auto leftSignal = signal(true);
    const auto rightSignal = signal(false);
    frame.leftTrigger = ProcessSide(
        block.leftTrigger, leftPosition, block.timestampUs,
        leftSignal.first, leftSignal.second, left_);
    frame.rightTrigger = ProcessSide(
        block.rightTrigger, rightPosition, block.timestampUs,
        rightSignal.first, rightSignal.second, right_);
}

void BuildVaderHapticPacket(uint8_t leftGrip, uint8_t rightGrip,
    uint8_t leftTrigger, uint8_t rightTrigger, uint8_t* out33)
{
    std::memset(out33, 0, 33);
    out33[0] = 0x00;
    out33[1] = 0x5A;
    out33[2] = 0xA5;
    out33[3] = 0x12;
    out33[4] = 0x06;
    out33[5] = leftGrip;
    out33[6] = rightGrip;
    out33[7] = leftTrigger;
    out33[8] = rightTrigger;
}

RumbleOutputBackend::RumbleOutputBackend(bool nullMode,
    int simulatedWriteDelayMs, TestWriteHook testWriteHook)
    : nullMode_(nullMode)
    , simulatedWriteDelayMs_(simulatedWriteDelayMs)
    , testWriteHook_(std::move(testWriteHook))
{
}

RumbleOutputBackend::~RumbleOutputBackend()
{
    Stop();
}

void RumbleOutputBackend::Start()
{
    writerThread_ = std::thread(&RumbleOutputBackend::WriterLoop, this);
}

void RumbleOutputBackend::Enqueue(const VaderHapticFrame& drive)
{
    if (writeFailed_.load(std::memory_order_relaxed)) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (writeFailed_.load(std::memory_order_relaxed)) {
        return;
    }
    if (hasPending_) {
        ++stats_.droppedStale;
        ++stats_.coalesced;
    }
    pending_ = drive;
    pendingForce_ = false;
    pendingEnqueueUs_ = NowUs();
    hasPending_ = true;
    ++stats_.enqueued;
    stats_.maxQueueDepth = std::max(stats_.maxQueueDepth, uint64_t{1});
    condition_.notify_one();
}

void RumbleOutputBackend::StopAllMotors()
{
    VaderHapticFrame zero;
    zero.timestampUs = NowUs();
    std::lock_guard<std::mutex> lock(mutex_);
    if (hasPending_) {
        ++stats_.droppedStale;
        ++stats_.coalesced;
    }
    pending_ = zero;
    pendingForce_ = true;
    pendingEnqueueUs_ = NowUs();
    hasPending_ = true;
    ++stats_.enqueued;
    stats_.maxQueueDepth = std::max(stats_.maxQueueDepth, uint64_t{1});
    condition_.notify_one();
}

void RumbleOutputBackend::Stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_) {
            return;
        }
        stop_ = true;
    }
    condition_.notify_all();
    if (writerThread_.joinable()) {
        writerThread_.join();
    }
}

void RumbleOutputBackend::ResetCounters()
{
    std::lock_guard<std::mutex> lock(mutex_);
    stats_ = OutputStats{};
    writeFailed_.store(false, std::memory_order_relaxed);
    lastWriteError_.store(0, std::memory_order_relaxed);
}

void RumbleOutputBackend::WriterLoop()
{
    while (true) {
        VaderHapticFrame drive;
        int64_t enqueueUs = 0;
        bool force = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [&] { return stop_ || hasPending_; });
            if (!hasPending_) {
                if (stop_) {
                    return;
                }
                continue;
            }
            drive = pending_;
            enqueueUs = pendingEnqueueUs_;
            force = pendingForce_;
            hasPending_ = false;
        }
        const int64_t beforeWriteUs = NowUs();
        stats_.latencyUs.push_back(
            static_cast<uint64_t>(std::max<int64_t>(0, beforeWriteUs - enqueueUs)));
        WritePacket(drive, force);
    }
}

bool RumbleOutputBackend::WritePacket(const VaderHapticFrame& drive, bool force)
{
    auto quantize = [](float value) {
        return static_cast<uint8_t>(std::min(255.0,
            std::max(0.0, static_cast<double>(value) * 255.0 + 0.5)));
    };
    const uint8_t leftGrip = quantize(drive.leftGrip);
    const uint8_t rightGrip = quantize(drive.rightGrip);
    const uint8_t leftTrigger = quantize(drive.leftTrigger);
    const uint8_t rightTrigger = quantize(drive.rightTrigger);
    const bool zero = leftGrip == 0 && rightGrip == 0 &&
        leftTrigger == 0 && rightTrigger == 0;
    const bool identical = hasSuccessfulPhysicalState_ &&
        lastSuccessfulLeftGrip_ == leftGrip &&
        lastSuccessfulRightGrip_ == rightGrip &&
        lastSuccessfulLeftTrigger_ == leftTrigger &&
        lastSuccessfulRightTrigger_ == rightTrigger;
    if (!force && identical) {
        if (zero) {
            ++stats_.suppressedRedundantZero;
        }
        else {
            ++stats_.suppressedRedundantState;
        }
        return true;
    }
    if (nullMode_) {
        if (simulatedWriteDelayMs_ > 0) {
            Sleep(static_cast<DWORD>(simulatedWriteDelayMs_));
        }
        return true;
    }
    bool success = false;
    DWORD writeError = ERROR_SUCCESS;
    if (testWriteHook_) {
        success = testWriteHook_(
            leftGrip, rightGrip, leftTrigger, rightTrigger);
        if (!success) {
            writeError = ERROR_WRITE_FAULT;
        }
    }
    else {
        uint8_t packet[33] = {};
        BuildVaderHapticPacket(
            leftGrip, rightGrip, leftTrigger, rightTrigger, packet);
        success = OverlappedWrite(
            deviceHandle_, packet, sizeof(packet), writeError);
    }
    ++stats_.written;
    if (!success) {
        ++stats_.writeFailures;
        lastWriteError_.store(writeError, std::memory_order_relaxed);
        writeFailed_.store(true, std::memory_order_relaxed);
        return false;
    }
    hasSuccessfulPhysicalState_ = true;
    lastSuccessfulLeftGrip_ = leftGrip;
    lastSuccessfulRightGrip_ = rightGrip;
    lastSuccessfulLeftTrigger_ = leftTrigger;
    lastSuccessfulRightTrigger_ = rightTrigger;
    if (zero) {
        ++stats_.zeroPackets;
    }
    return true;
}

bool RunDryRun(const DryRunConfig& config, DryRunReport& report,
    std::string& error)
{
    std::vector<CaptureTraceRecord> records;
    uint32_t traceVersion = 0;
    if (!ReadCaptureTrace(config.tracePath, records, traceVersion, error)) {
        return false;
    }

    HapticRenderer renderer(config.renderer);
    RumbleOutputBackend backend(true, config.simulatedWriteDelayMs);
    backend.Start();

    std::vector<uint64_t> processingNs;
    struct Bin {
        double pcmRmsSum = 0.0;
        uint64_t pcmFrames = 0;
        uint64_t allZero84 = 0;
        uint64_t blocks84 = 0;
        float maxDrive = 0.0f;
    };
    std::vector<Bin> bins(300);
    std::vector<ShotDryRun> shots(config.shotMarkersUs.size());
    for (size_t index = 0; index < shots.size(); ++index) {
        shots[index].markerUs = config.shotMarkersUs[index];
    }

    uint32_t currentGeneration = 0;
    int64_t lastBlockUs = 0;
    for (const auto& record : records) {
        if (record.kind != CaptureRecordKind::Frame) {
            continue;
        }
        if (record.type == 0x83 && record.payload.size() == 2396) {
            const uint8_t* pcm = record.payload.data() + 476;
            double sumSq = 0.0;
            int32_t peak = 0;
            for (size_t sample = 0; sample < 480; ++sample) {
                const int16_t left = Load16LE(pcm + sample * 4);
                const int16_t right = Load16LE(pcm + sample * 4 + 2);
                sumSq += static_cast<double>(left) * left;
                sumSq += static_cast<double>(right) * right;
                peak = std::max(peak, std::abs(static_cast<int>(left)));
                peak = std::max(peak, std::abs(static_cast<int>(right)));
            }
            const double rms = std::sqrt(sumSq / (480.0 * 2.0));
            const int64_t bin = record.timestampUs / 1000000;
            if (bin >= 0 && bin < 300) {
                bins[static_cast<size_t>(bin)].pcmRmsSum += rms;
                ++bins[static_cast<size_t>(bin)].pcmFrames;
            }
            (void)peak;
            continue;
        }
        if (record.type != 0x84 || record.payload.size() != 474) {
            continue;
        }

        if (currentGeneration == 0) {
            currentGeneration = record.generation;
        }
        else if (record.generation != currentGeneration) {
            renderer.Reset();
            backend.StopAllMotors();
            ++report.generationResets;
            currentGeneration = record.generation;
        }

        if (lastBlockUs != 0 &&
            record.timestampUs - lastBlockUs > config.watchdogMaxAgeUs) {
            bool triggered = false;
            const VaderHapticFrame zero =
                renderer.WatchdogZero(record.timestampUs,
                    config.watchdogMaxAgeUs, triggered);
            if (triggered) {
                backend.Enqueue(zero);
                ++report.watchdogZeroEvents;
            }
        }
        lastBlockUs = record.timestampUs;

        DualSenseHapticsBlock block;
        if (!DecodeDualSenseHapticsBlock(record.payload.data(),
                record.payload.size(), block)) {
            error = "decoder rejected a 474-byte 0x84 frame";
            backend.StopAllMotors();
            backend.Stop();
            return false;
        }
        block.generation = record.generation;
        block.sequence = record.sequence;
        block.timestampUs = record.timestampUs;
        ++report.blocksDecoded;
        bool allZero = true;
        for (const auto& sample : block.samples) {
            if (sample.left != 0 || sample.right != 0) {
                allZero = false;
                break;
            }
        }
        if (allZero) {
            ++report.allZeroBlocks;
        }
        else {
            ++report.activeBlocks;
        }

        const auto start = std::chrono::steady_clock::now();
        const VaderHapticFrame drive = renderer.Process(block);
        if (allZero &&
            (drive.leftGrip > 0.0f || drive.rightGrip > 0.0f)) {
            ++report.zeroBlocksWithNonzeroDrive;
        }
        const auto end = std::chrono::steady_clock::now();
        processingNs.push_back(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - start)
                .count()));

        const int64_t bin = record.timestampUs / 1000000;
        if (bin >= 0 && bin < 300) {
            Bin& target = bins[static_cast<size_t>(bin)];
            ++target.blocks84;
            target.allZero84 += allZero ? 1u : 0u;
            target.maxDrive = std::max(target.maxDrive,
                std::max(drive.leftGrip, drive.rightGrip));
        }

        for (auto& shot : shots) {
            if (record.timestampUs >= shot.markerUs - 250000 &&
                record.timestampUs <= shot.markerUs + 1000000) {
                ++shot.blocksInWindow;
                shot.maxLow = std::max(shot.maxLow, drive.leftGrip);
                shot.maxHigh = std::max(shot.maxHigh, drive.rightGrip);
                if (!shot.firstNonZeroFound &&
                    (drive.leftGrip > 0.0f ||
                        drive.rightGrip > 0.0f)) {
                    shot.firstNonZeroFound = true;
                    shot.firstNonZeroDriveOffsetUs =
                        record.timestampUs - shot.markerUs;
                }
            }
        }
        backend.Enqueue(drive);
    }

    // Watchdog at end of trace: source stopped, motors must zero.
    {
        bool triggered = false;
        const VaderHapticFrame zero = renderer.WatchdogZero(
            lastBlockUs + config.watchdogMaxAgeUs + 1000,
            config.watchdogMaxAgeUs, triggered);
        if (triggered) {
            backend.Enqueue(zero);
            ++report.watchdogZeroEvents;
        }
    }

    // Generation/reset simulation: reset must clear renderer state to zero.
    {
        renderer.Reset();
        DualSenseHapticsBlock synthetic;
        synthetic.generation = 2;
        synthetic.sequence = 0;
        synthetic.timestampUs = lastBlockUs + config.watchdogMaxAgeUs + 2000;
        const VaderHapticFrame drive = renderer.Process(synthetic);
        report.resetClearedDrive =
            drive.leftGrip == 0.0f && drive.rightGrip == 0.0f &&
            drive.leftTrigger == 0.0f && drive.rightTrigger == 0.0f;
        backend.Enqueue(drive);
        ++report.generationResets;
    }

    backend.StopAllMotors();
    backend.Stop();

    report.maxQueueDepth = backend.stats().maxQueueDepth;
    report.droppedStale = backend.stats().droppedStale;
    report.coalesced = backend.stats().coalesced;
    report.writtenCount = backend.stats().written;
    report.writeFailures = backend.stats().writeFailures;
    report.watchdogZeroEvents += backend.stats().watchdogZeroEvents;
    report.zeroPackets = backend.stats().zeroPackets;
    report.processingP50Ns = static_cast<uint64_t>(Percentile(processingNs, 0.50));
    report.processingP95Ns = static_cast<uint64_t>(Percentile(processingNs, 0.95));
    report.processingP99Ns = static_cast<uint64_t>(Percentile(processingNs, 0.99));
    uint64_t processingTotal = 0;
    for (const uint64_t value : processingNs) {
        processingTotal += value;
    }
    report.processingNsTotal = processingTotal;
    report.latencyP50Us = static_cast<uint64_t>(
        Percentile(backend.stats().latencyUs, 0.50));
    report.latencyP95Us = static_cast<uint64_t>(
        Percentile(backend.stats().latencyUs, 0.95));
    report.latencyP99Us = static_cast<uint64_t>(
        Percentile(backend.stats().latencyUs, 0.99));
    report.shots = std::move(shots);
    report.backlogBounded = backend.stats().maxQueueDepth <= 1;

    // PCM-dominant / rear-quiet bins must not produce strong grip drive.
    std::vector<size_t> pcmRanked;
    for (size_t index = 0; index < bins.size(); ++index) {
        if (bins[index].pcmFrames > 0 && bins[index].blocks84 > 0) {
            pcmRanked.push_back(index);
        }
    }
    std::sort(pcmRanked.begin(), pcmRanked.end(),
        [&](size_t left, size_t right) {
            const double leftRms = bins[left].pcmRmsSum / bins[left].pcmFrames;
            const double rightRms = bins[right].pcmRmsSum / bins[right].pcmFrames;
            if (leftRms != rightRms) {
                return leftRms > rightRms;
            }
            return left < right;
        });
    int checked = 0;
    for (const size_t index : pcmRanked) {
        if (checked >= 5) {
            break;
        }
        const Bin& bin = bins[index];
        const double quietFraction =
            static_cast<double>(bin.allZero84) /
            static_cast<double>(bin.blocks84);
        if (quietFraction > 0.5) {
            report.pcmDominantMaxDrive =
                std::max(report.pcmDominantMaxDrive,
                    static_cast<double>(bin.maxDrive));
            ++report.pcmDominantBins;
        }
        ++checked;
    }

    std::ostringstream summary;
    summary << "M3.4 dry-run gate\n";
    summary << "blocks_decoded: " << report.blocksDecoded << "\n";
    summary << "all_zero_blocks: " << report.allZeroBlocks << "\n";
    summary << "active_blocks: " << report.activeBlocks << "\n";
    summary << "zero_blocks_with_nonzero_drive: "
            << report.zeroBlocksWithNonzeroDrive << "\n";
    summary << "generation_resets: " << report.generationResets << "\n";
    summary << "reset_cleared_drive: "
            << (report.resetClearedDrive ? "yes" : "no") << "\n";
    summary << "processing_p50_ns: " << report.processingP50Ns << "\n";
    summary << "processing_p95_ns: " << report.processingP95Ns << "\n";
    summary << "processing_p99_ns: " << report.processingP99Ns << "\n";
    summary << "enqueue_to_write_p50_us: " << report.latencyP50Us << "\n";
    summary << "enqueue_to_write_p95_us: " << report.latencyP95Us << "\n";
    summary << "enqueue_to_write_p99_us: " << report.latencyP99Us << "\n";
    summary << "max_queue_depth: " << report.maxQueueDepth << "\n";
    summary << "dropped_stale: " << report.droppedStale << "\n";
    summary << "write_failures: " << report.writeFailures << "\n";
    summary << "watchdog_zero_events: " << report.watchdogZeroEvents << "\n";
    summary << "zero_packets_written: " << report.zeroPackets << "\n";
    summary << "backlog_bounded: " << (report.backlogBounded ? "yes" : "no")
            << "\n";
    summary << "pcm_dominant_quiet_84_bins: " << report.pcmDominantBins
            << "\n";
    summary << "pcm_dominant_max_drive: " << report.pcmDominantMaxDrive
            << "\n";
    for (const auto& shot : report.shots) {
        summary << "shot " << shot.markerUs << " first_nonzero_offset_us="
                << shot.firstNonZeroDriveOffsetUs
                << " found=" << (shot.firstNonZeroFound ? "yes" : "no")
                << " max_low=" << shot.maxLow << " max_high=" << shot.maxHigh
                << " blocks_in_window=" << shot.blocksInWindow << "\n";
    }
    report.summary = summary.str();
    return true;
}

} // namespace m3
} // namespace vaderhaptics
