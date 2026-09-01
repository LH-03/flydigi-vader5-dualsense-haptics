#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vaderhaptics {
namespace m3 {

// M3 primary source contract (pinned VIIPER v0.0.9):
// - V5 feedback object: 474 bytes; Bluetooth combined haptics offset 78 from
//   carrier offset 76 => payload bytes 154..217 inclusive (64 bytes).
// - 64 bytes = 32 stereo signed-8-bit frames at 3 kHz (~10.667 ms interval).

struct DualSenseHapticsSample {
    int8_t left = 0;
    int8_t right = 0;
};

struct DualSenseTriggerEffect {
    uint8_t mode = 0;
    uint8_t startResistance = 0;
    uint8_t effectForce = 0;
    uint8_t rangeForce = 0;
    uint8_t nearReleaseStrength = 0;
    uint8_t nearMiddleStrength = 0;
    uint8_t pressedStrength = 0;
    uint8_t frequency = 0;

    bool operator==(const DualSenseTriggerEffect&) const = default;
};

struct DualSenseHapticsBlock {
    uint32_t generation = 0;
    uint32_t sequence = 0;
    int64_t timestampUs = 0;
    DualSenseHapticsSample samples[32] = {};
    DualSenseTriggerEffect leftTrigger;
    DualSenseTriggerEffect rightTrigger;
};

// Final renderer/backend boundary. The four values are composed before the
// single physical write so grip and trigger paths can never race each other.
struct VaderHapticFrame {
    float leftGrip = 0.0f;     // byte 5 / G0, logical left grip lane
    float rightGrip = 0.0f;    // byte 6 / G1, logical right grip lane
    float leftTrigger = 0.0f;  // byte 7 / T0
    float rightTrigger = 0.0f; // byte 8 / T1
    int64_t timestampUs = 0;
};

// Apply the final global safety cap after grip and trigger paths have been
// composed. This is the last renderer-side operation before quantization.
void LimitVaderHapticFrame(VaderHapticFrame& frame, double limiterCap);

// Decode a validated 474-byte 0x84 feedback object. Returns false if size is
// not exactly 474. The 64-byte region is parsed as interleaved L/R s8.
bool DecodeDualSenseHapticsBlock(const uint8_t* feedback474, size_t size,
    DualSenseHapticsBlock& out);

enum class RendererPolicy {
    StereoEnvelope = 0,
    SpectralMono = 1,
};

enum class FidelityModel {
    FirstPlayableV1 = 1,
    TemporalContrastV2 = 2,
};

struct RendererConfig {
    RendererPolicy policy = RendererPolicy::StereoEnvelope;
    // Old/missing configs remain exactly first-playable. The active production
    // candidate opts into V2 explicitly with fidelity_model=2.
    FidelityModel fidelityModel = FidelityModel::FirstPlayableV1;
    double sourceGain = 1.0;     // normalize the game's practical PCM range
    double weakLift = 0.04;      // smooth lift scale (fraction of full scale)
    double liftStrength = 0.25;  // max additive lift at exact zero
    double gamma = 0.85;         // nonlinear mapping exponent
    double softKnee = 0.15;      // gentle saturating curve coefficient
    double attackS = 0.012;
    double releaseS = 0.040;
    double transientBoost = 0.35;
    double textureFloor = 0.16;          // quiet floor for smooth sustained energy
    double textureRoughnessGain = 0.24;  // preserve changing surface texture
    double roughnessReference = 0.055;   // mean sample delta treated as rough
    double relativeMotionReference = 0.38; // V2 amplitude-normalized motion
    double slowContextS = 0.140;         // V2 sustained-energy context
    double noveltyMix = 0.80;            // V2 embedded-onset contribution
    double impactGain = 1.0;             // maximum normalized impact contribution
    double impactThreshold = 0.018;      // reject tiny block-to-block onsets
    double impactFullScale = 0.22;       // onset that reaches full impact target
    double impactReleaseS = 0.050;       // finite impact tail, not a held buzz
    double triggerFallbackGain = 0.38;   // PCM-onset fallback when metadata is off
    double triggerFallbackCap = 96.0 / 255.0;
    uint8_t triggerFallbackDeadzone = 32;
    double triggerFallbackOnsetGate = 0.12;
    double triggerFallbackPulseS = 0.055;
    double triggerFallbackRefractoryS = 0.085;
    double triggerFallbackGamma = 0.68;  // front-loaded impulse strength curve
    double limiterCap = 1.0;
    double minDrive = 0.0;       // calibrated later; 0 keeps exact-zero blocks zero
    double lowCutoffHz = 250.0;  // spectral-mono low/high separation
};

// Load config/m3_calibration.json into renderer tuning. Missing or invalid
// configuration fails safe: minDrive stays 0 and the limiter cap is clamped
// to 128/255 (never full intensity). A valid optional limiter_cap_amplitude
// can lower that cap for all four final output lanes. Exact-zero source blocks
// are unaffected (min-drive applies only to nonzero rendered drive).
bool LoadCalibrationConfig(const std::string& path, RendererConfig& config,
    std::string& error);

class HapticRenderer {
public:
    explicit HapticRenderer(const RendererConfig& config);

    // Zero all internal state (generation reset, disconnect, shutdown).
    void Reset();

    // Process one 0x84 haptics block; returns the target grip drive.
    // An all-zero source block returns an exact-zero target.
    VaderHapticFrame Process(const DualSenseHapticsBlock& block);

    // Watchdog: if nowUs - lastSourceUs > maxAgeUs, returns a zero drive and
    // flags the event; otherwise returns the last drive unchanged.
    VaderHapticFrame WatchdogZero(int64_t nowUs, int64_t maxAgeUs,
        bool& triggered);

    int64_t lastSourceUs() const { return lastSourceUs_; }

private:
    struct ChannelSignal {
        double energy = 0.0;
        double roughness = 0.0;
        double crest = 0.0;
    };

    double ProcessChannel(const ChannelSignal& signal, double& envState,
        double& transientState, double& previousEnergy,
        double& slowContext);
    void ProcessBlockPolicies(const DualSenseHapticsBlock& block,
        ChannelSignal& left, ChannelSignal& right);

    RendererConfig config_;
    double dt_ = 0.0106666667;
    double attackCoeff_ = 0.0;
    double releaseCoeff_ = 0.0;
    double impactReleaseCoeff_ = 0.0;
    double slowContextCoeff_ = 0.0;
    double alphaLow_ = 0.0;
    double envLeft_ = 0.0;
    double envRight_ = 0.0;
    double transientLeft_ = 0.0;
    double transientRight_ = 0.0;
    double previousEnergyLeft_ = 0.0;
    double previousEnergyRight_ = 0.0;
    double slowContextLeft_ = 0.0;
    double slowContextRight_ = 0.0;
    double monoLow_ = 0.0;
    int previousLeftSample_ = 0;
    int previousRightSample_ = 0;
    double previousHighSample_ = 0.0;
    double envLow_ = 0.0;
    double envHigh_ = 0.0;
    double transientLow_ = 0.0;
    double transientMono_ = 0.0;
    double previousEnergyLow_ = 0.0;
    double previousEnergyHigh_ = 0.0;
    double slowContextLow_ = 0.0;
    double slowContextHigh_ = 0.0;
    int64_t lastSourceUs_ = 0;
    VaderHapticFrame lastDrive_;
};

enum class TriggerEffectClass {
    Off,
    Continuous,
    Breakpoint,
    Weapon,
    Oscillation,
    Unsupported,
};

TriggerEffectClass ClassifyTriggerEffect(uint8_t mode);

// Small stateful semantic substitution: DS5 adaptive-trigger metadata is
// converted into bounded trigger vibration, gated by the corresponding real
// trigger position. It never attempts to emulate resistance.
class TriggerSemanticAdapter {
public:
    explicit TriggerSemanticAdapter(
        const RendererConfig& config = RendererConfig{});
    void Reset();
    void Process(const DualSenseHapticsBlock& block, uint8_t leftPosition,
        uint8_t rightPosition, VaderHapticFrame& frame);

private:
    struct SideState {
        DualSenseTriggerEffect lastEffect;
        uint8_t lastPosition = 0;
        int64_t pulseUntilUs = 0;
        double previousSourceEnergy = 0.0;
        double fallbackPulse = 0.0;
        int64_t fallbackPulseStartUs = 0;
        int64_t fallbackPulseUntilUs = 0;
        int64_t fallbackRearmUs = 0;
    };

    float ProcessSide(const DualSenseTriggerEffect& effect, uint8_t position,
        int64_t nowUs, double sourceEnergy, double sourceCrest,
        SideState& state);

    RendererConfig config_;
    double fallbackReleaseCoeff_ = 0.0;
    SideState left_;
    SideState right_;
};

// Accepted four-channel Vader packet (33-byte output report):
// 00 5A A5 12 06 <leftGrip> <rightGrip> <leftTrigger> <rightTrigger> ...
void BuildVaderHapticPacket(uint8_t leftGrip, uint8_t rightGrip,
    uint8_t leftTrigger, uint8_t rightTrigger, uint8_t* out33);

struct OutputStats {
    uint64_t enqueued = 0;
    uint64_t written = 0;
    uint64_t droppedStale = 0;   // newest-wins overwrite of unsent target
    uint64_t coalesced = 0;      // same event as droppedStale, explicit label
    uint64_t writeFailures = 0;
    uint64_t zeroPackets = 0;
    uint64_t suppressedRedundantZero = 0;
    uint64_t suppressedRedundantState = 0;
    uint64_t watchdogZeroEvents = 0;
    uint64_t maxQueueDepth = 0;
    uint64_t resetEvents = 0;
    std::vector<uint64_t> latencyUs; // enqueue -> write start
};

// Bounded newest-wins output backend. Capacity is exactly one pending target;
// a newer drive replaces the pending one (drop + coalesce counters). A
// dedicated writer thread may block on HID write without blocking callers.
// Null mode simulates the write (optionally with an artificial delay) and
// never opens a device; used for the M3.4 dry-run gate.
class RumbleOutputBackend {
public:
    using TestWriteHook = std::function<bool(uint8_t leftGrip,
        uint8_t rightGrip, uint8_t leftTrigger, uint8_t rightTrigger)>;

    RumbleOutputBackend(bool nullMode, int simulatedWriteDelayMs,
        TestWriteHook testWriteHook = {});
    ~RumbleOutputBackend();

    void Start();
    void Enqueue(const VaderHapticFrame& drive);
    void StopAllMotors(); // zero target + flush
    void Stop();          // join writer, no output after
    void ResetCounters();
    void SetDeviceHandle(void* handle) { deviceHandle_ = handle; }
    bool writeFailed() const
    {
        return writeFailed_.load(std::memory_order_relaxed);
    }
    uint32_t lastWriteError() const
    {
        return lastWriteError_.load(std::memory_order_relaxed);
    }

    const OutputStats& stats() const { return stats_; }
    bool nullMode() const { return nullMode_; }

private:
    void WriterLoop();
    bool WritePacket(const VaderHapticFrame& drive, bool force);

    bool nullMode_;
    int simulatedWriteDelayMs_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool stop_ = false;
    bool hasPending_ = false;
    bool pendingForce_ = false;
    VaderHapticFrame pending_;
    int64_t pendingEnqueueUs_ = 0;
    void* deviceHandle_ = nullptr;
    TestWriteHook testWriteHook_;
    bool hasSuccessfulPhysicalState_ = false;
    uint8_t lastSuccessfulLeftGrip_ = 0;
    uint8_t lastSuccessfulRightGrip_ = 0;
    uint8_t lastSuccessfulLeftTrigger_ = 0;
    uint8_t lastSuccessfulRightTrigger_ = 0;
    std::thread writerThread_;
    std::atomic<bool> writeFailed_{false};
    std::atomic<uint32_t> lastWriteError_{0};
    OutputStats stats_;
};

struct DryRunConfig {
    std::string tracePath;
    RendererConfig renderer;
    int simulatedWriteDelayMs = 20;
    int64_t watchdogMaxAgeUs = 120000;
    std::vector<int64_t> shotMarkersUs = {
        283424115LL, 287446112LL, 291696104LL, 295274106LL};
};

struct ShotDryRun {
    int64_t markerUs = 0;
    int64_t firstNonZeroDriveOffsetUs = 0;
    bool firstNonZeroFound = false;
    float maxLow = 0.0f;
    float maxHigh = 0.0f;
    int blocksInWindow = 0;
};

struct DryRunReport {
    uint64_t blocksDecoded = 0;
    uint64_t allZeroBlocks = 0;
    uint64_t activeBlocks = 0;
    uint64_t zeroBlocksWithNonzeroDrive = 0;
    uint64_t generationResets = 0;
    uint64_t maxQueueDepth = 0;
    uint64_t droppedStale = 0;
    uint64_t coalesced = 0;
    uint64_t writtenCount = 0;
    uint64_t writeFailures = 0;
    uint64_t watchdogZeroEvents = 0;
    uint64_t zeroPackets = 0;
    uint64_t processingNsTotal = 0;
    uint64_t processingP50Ns = 0;
    uint64_t processingP95Ns = 0;
    uint64_t processingP99Ns = 0;
    uint64_t latencyP50Us = 0;
    uint64_t latencyP95Us = 0;
    uint64_t latencyP99Us = 0;
    std::vector<ShotDryRun> shots;
    double pcmDominantMaxDrive = 0.0; // max drive in top-PCM/quiet-0x84 bins
    uint64_t pcmDominantBins = 0;
    bool resetClearedDrive = false;
    bool backlogBounded = true;
    std::string summary;
};

// Replay a VHTC trace through the actual decoder + renderer + null backend.
// No device is opened and no physical output is produced.
bool RunDryRun(const DryRunConfig& config, DryRunReport& report,
    std::string& error);

} // namespace m3
} // namespace vaderhaptics
