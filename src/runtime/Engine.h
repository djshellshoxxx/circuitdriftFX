// CircuitDrift runtime: minimal integrated engine (P03 integration slice).
// Composition per host sample: timestamped events -> entry sanitize/range guard -> input gain ->
// CircuitRuntime (oversampled MNA, thermal clock, latched solver fallback) -> OutputStage
// (level-matched blend -> trim -> HPF -> oversampled soft clip -> host bypass -> containment).
// Owner: cd_runtime. Self-contained on cd_core/cd_dsp; control/state/blueprint wiring is later work.
//
// Lifetime/generations: buildPrepared() (non-audio) builds a complete candidate with a new
// generation number; tryInstallPrepared() (audio-safe, noexcept) swaps it in only if its generation
// is newer than the active one and hands the previous engine back through the same handle so the
// caller reclaims it off the audio thread. prepare() = build + install + reclaim, for use while
// audio is stopped. A failed build leaves the active engine untouched.
// Quality is fixed per prepared engine; live quality transitions with the 20 ms crossfade and the
// host latency-change handshake are NOT implemented (later work). Zero Latency is a separate
// preparation mode (1x, no padding, native-rate soft clip).
// Clocks: the sample counter advances only by processed frames; a zero-frame call applies events
// only. A changed ProcessContext::epoch is counted (no network/pitch history exists yet to clear);
// capacitor/thermal state persists across epochs.
// Unsupported layouts/rates are explicit: channels 1|2 (in == out count), 44.1-192 kHz, double
// samples only (32-bit host conversion belongs to adapters). A context whose sampleRate or channel
// count differs from the prepared engine yields silence and a status flag; clocks do not advance.
// Threading: process()/reset()/tryInstallPrepared() noexcept, no allocation/locks/I/O/logging.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "dsp/Oversampling.h"
#include "runtime/CircuitRuntime.h"
#include "runtime/OutputStage.h"

namespace cd::runtime {

enum class QualityMode : uint8_t { Eco, Studio, Render, Manual };

struct QualitySettings {
    QualityMode mode = QualityMode::Studio;
    int manualFactor = 4;                        // Manual: 1,2,4,8,16 (1x/2x are Draft)
    dsp::OsFamily family = dsp::OsFamily::Fir;   // Manual only; IIR is a labelled exception
    bool zeroLatency = false;                    // explicit Zero Latency preparation mode
};

struct QualityInfo {
    int factor = 0;
    dsp::OsFamily family = dsp::OsFamily::Fir;
    int maxNewton = 0;
    bool draft = false;              // factor < 4: Draft antialiasing (spec exception)
    bool familyException = false;    // IIR: exception to the FIR production default
    bool zeroLatency = false;
    int circuitLatency = 0, clipLatency = 0, latencySamples = 0;
};

struct EngineConfig {
    double sampleRate = 48000;
    int maxBlock = 512;
    int channels = 2;
    QualitySettings quality{};
    // Netlist, synthetic I/O and thermal contract. sampleRate/maxBlock/channels/os/maxNewton/
    // padToFamilyMax are overwritten from the fields above.
    CircuitConfig circuit = syntheticDiodeClipper();
    OutputParams initial{};
    int maxNewtonOverride = 0;       // 0 = from quality; 1..12 for diagnostics/tests
};

struct ProcessContext {
    double sampleRate = 0;
    int64_t sampleStart = 0;
    uint64_t epoch = 0;
    uint32_t frames = 0;
    bool offline = false;
};

enum class ParamTarget : uint8_t { InputGainDb, OutputTrimDb, Mix, AutoGain, HpfEnabled, HpfCutoffHz, SoftClipEnabled, HostBypass, Count };

struct ParamEvent {
    uint32_t offset = 0;             // sample offset in this call; > frames applies at the end
    ParamTarget target = ParamTarget::Mix;
    double value = 0;                // booleans: value >= 0.5
};

struct AudioBuses {
    int channels = 0;
    const double* const* in = nullptr;
    double* const* out = nullptr;    // may equal in (in-place)
};

struct ProcessBlock {
    ProcessContext ctx{};
    AudioBuses buses{};
    const ParamEvent* events = nullptr;
    uint32_t eventCount = 0;
};

namespace status {
constexpr uint32_t kOk = 0, kNotPrepared = 1u << 0, kRateMismatch = 1u << 1, kLayoutMismatch = 1u << 2,
                   kInputSanitized = 1u << 3, kContainment = 1u << 4, kCircuitFallback = 1u << 5,
                   kResetRequested = 1u << 6, kEventsCoalesced = 1u << 7;
}

struct ProcessResult {
    uint32_t frames = 0;
    uint32_t status = status::kOk;
};

struct EngineDiagnostics {
    int64_t samples = 0;
    uint64_t epoch = 0, generation = 0;
    long epochChanges = 0, inputNonFinite = 0, inputClamped = 0, eventsCoalesced = 0, staleInstalls = 0;
    CircuitRuntime::Diagnostics circuit{};
    OutputStage::Diagnostics output{};
};

// A fully prepared engine instance (circuit + output stage + slice buffers).
class PreparedEngine {
public:
    uint64_t generation() const noexcept { return generation_; }
    const QualityInfo& quality() const noexcept { return quality_; }
    const CircuitRuntime& circuit() const noexcept { return circuit_; }
    const OutputStage& output() const noexcept { return output_; }
private:
    friend class Engine;
    CircuitRuntime circuit_;
    OutputStage output_;
    QualityInfo quality_{};
    double fs_ = 0;
    int channels_ = 0, maxBlock_ = 0;
    uint64_t generation_ = 0;
    std::vector<double> raw_, gained_, wet_, dry_;
};

class Engine {
public:
    static constexpr uint32_t kMaxEventsPerCall = 4096;
    static constexpr double kEntryLimit = 1e6;   // entry range guard (+120 dBFS), keeps state finite

    // Non-audio. Returns nullptr with error on failure; active engine unaffected.
    std::unique_ptr<PreparedEngine> buildPrepared(const EngineConfig& cfg, std::string& error);
    // Audio-safe swap. Rejects null or stale (generation <= active) handles, leaving them intact.
    // On success the handle holds the previous engine (possibly null) for off-audio reclamation.
    bool tryInstallPrepared(std::unique_ptr<PreparedEngine>& handle) noexcept;
    // Non-audio convenience: build + install + destroy the previous engine.
    bool prepare(const EngineConfig& cfg, std::string& error);

    ProcessResult process(const ProcessBlock& block) noexcept;
    void reset() noexcept;

    bool prepared() const noexcept { return active_ != nullptr; }
    int latencySamples() const noexcept { return active_ ? active_->quality_.latencySamples : 0; }
    QualityInfo quality() const noexcept { return active_ ? active_->quality_ : QualityInfo{}; }
    EngineDiagnostics diagnostics() const noexcept;
    const PreparedEngine* active() const noexcept { return active_.get(); }

    static bool resolveQuality(const QualitySettings& q, int maxNewtonOverride, dsp::Oversampler::Spec& spec,
                               QualityInfo& info) noexcept;

private:
    void applyEvent(const ParamEvent& e) noexcept;
    void runSlice(PreparedEngine& p, const double* const* in, double* const* out, int offset, int n, uint32_t& st) noexcept;

    std::unique_ptr<PreparedEngine> active_;
    uint64_t nextGeneration_ = 0;
    int64_t samples_ = 0;
    uint64_t epoch_ = 0;
    bool haveEpoch_ = false;
    long epochChanges_ = 0, inputNonFinite_ = 0, inputClamped_ = 0, eventsCoalesced_ = 0, staleInstalls_ = 0;
};

}  // namespace cd::runtime
