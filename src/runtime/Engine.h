// CircuitDrift runtime: integrated engine (P03 integration slice + resume packets 1-4 wiring).
// Composition per host sample: timestamped events -> entry sanitize/range guard -> input gain ->
// macro/lifecycle/creative-motion effective values -> transient + sidechain detectors ->
// CircuitRuntime (oversampled MNA + reduced supply reservoir, power accumulation, 1 kHz thermal
// update with temperature feedback, latched solver fallback; built-in circuit or crossfaded
// blueprint graph paths) -> wet creative stages (Age shelf, Drift cutoff motion, optional pitch-
// drift delay with dry aligned, Analog Noise) -> OutputStage (level-matched blend -> trim -> HPF
// -> oversampled soft clip -> host bypass -> containment).
// Owner: cd_runtime. Depends on cd_core/cd_dsp/cd_control/cd_state/cd_blueprint; never on UI/JUCE.
// Every circuit, supply, tempco and creative numeric is a labelled SYNTHETIC fixture (G01 open).
//
// Lifetime/generations: buildPrepared() (non-audio) builds a complete candidate with a new
// generation number; tryInstallPrepared() (audio-safe, noexcept) swaps it in only if its generation
// is newer than the active one and hands the previous engine back through the same handle so the
// caller reclaims it off the audio thread. prepare() = build + install + reclaim, for use while
// audio is stopped. A failed build leaves the active engine untouched. Installing a prepared
// engine whose configuration no longer matches the active graph path returns to the built-in
// circuit (graph processors are released, never destroyed here).
//
// Live quality transitions (resume packet 3, T25 part): a new quality is prepared OFF-THREAD with
// buildPrepared(); requestQuality() (audio-safe) then either
//  * Crossfades: same Fs/channels/slice size, same phase family (FIR/IIR), same Zero Latency mode
//    and the same declared (padded) latency. The incoming path adopts the macro runtime, output
//    parameter targets and slow physical state (temperatures, 1 kHz phase, rail volts, feedback
//    parameters) of the active path, pre-rolls for latencySamples() samples (its delay lines fill,
//    inaudible), then old/new outputs are crossfaded over 20 ms with linear equal-sum weights
//    (1-x, x). Latency never changes. The finished old engine is handed back via takeRetired() for
//    off-audio reclamation.
//  * Stages: a phase-family change (FIR<->IIR), a Zero Latency toggle, a pitch-delay toggle or any
//    other declared-latency change is NEVER applied while processing: the candidate becomes the
//    pending engine, latencyChangePending() is true, and the active quality keeps running until the
//    host deactivates and reactivates (activatePending(), non-audio, audio stopped; then report
//    latencySamples() to the host). qualityStatus() labels active, incoming and pending quality
//    separately; the engine never changes quality on its own (quality host IDs 1012-1015 and
//    pitchDrift.enabled 1016 are counted as requests for the adapter's worker, not applied).
//  * Rejects: no active engine, a transition already running, graphs attached, or mismatched
//    Fs/channels/slice size.
// Zero Latency is a separate preparation mode (1x, no padding, native-rate soft clip, no pitch delay).
// Clocks: the sample counter advances only by processed frames; a zero-frame call applies events
// only. A changed ProcessContext::epoch is counted (no network/pitch history exists yet to clear);
// capacitor/thermal state persists across epochs.
// Unsupported layouts/rates are explicit: channels 1|2 (in == out count), 44.1-192 kHz, double
// samples only (32-bit host conversion belongs to adapters). A context whose sampleRate or channel
// count differs from the prepared engine yields silence and a status flag; clocks do not advance.
// External sidechain: AudioBuses::side (1 or 2 channels, or null = disconnected). It feeds ONLY the
// SidechainLoadDetector (I_ext into the reservoir); it is never mixed into any audio path.
//
// Transaction events: process(ctx, buses, events) / processQueued(ctx, buses, queue) apply
// cd::state::TransactionEvent Params at their sampleOffset (the call is split at every event
// offset and at maxBlock). The queue is drained ONCE per process call with frames = call frames,
// so queue offsets are relative to the call (the call is the queue's consuming slice). Host IDs
// map through the cd::core registry (toPlain) to: macro.age/sag/drift/warmth, analogNoise.enabled,
// input.gain, output.gain (trim), mix, gainMatch.enabled (auto-gain), output.softClip,
// hpf.enabled, hpf.frequency, sidechain.source/depth/hpf, transient.preserve, slot.<role>.lifecycle
// and slot.<role>.scenario; quality.* and pitchDrift.enabled are counted requests (see above).
// Other registry IDs are counted as unsupported, unregistered IDs as unknown, the other A/B slot's
// IDs as otherSlot; none change audio. ConfigPublish events are counted only: graph installs are
// driven by the GraphInstaller pool (newest publish wins). Note events are counted (no voices).
// Macros: per-sample cd::control::MacroSmoother (20 ms) feeding cd::control::evaluate(base,
// macros, bindings, timeline offsets, creative motion q) whenever a smoothed value or q changes;
// effective values are never written back to base or to host parameters. Bindings are what the
// SYNTHETIC circuit can consume: Warmth -> pre-stage drive dB on the circuit input (wet only);
// Age -> RBJ high shelf -6*a dB at 6 kHz, Q 0.707 on the wet path (creative tilt) and, with Analog
// Noise on, seeded Gaussian noise at -90+24a dBFS RMS added to the wet path before the blend;
// Sag -> reservoir C_eff and the composeLoadCurrent load (only when CircuitConfig::supply is
// enabled; the engine derives hasSag/C_base/C_min from that supply manifest, any Sag fields of
// macroBindings are replaced); Drift -> temperature-law sensitivity 1+4d (circuits with tempco
// feedback elements), creative cutoff motion +-0.15*d*q octaves on the creative low-pass and the
// optional pitch-drift delay deviation (the engine sets hasDrift when any of these exists).
// transient.preserve (amount) and sidechain.depth use 20 ms smoothing; sidechain.hpf re-prepares the
// detector (envelope restarts). Without a supply, Sag/sidechain/transient IDs are counted unsupported
// and neutral. Lifecycle: slot lifecycle/scenario drive a cd::blueprint::LifecycleTimeline whose
// condition becomes evaluate()'s timeline offsets (telemetry only for the synthetic circuits).
// Creative motion: two seeded CreativeMotion streams (100 Hz clock, deriveSeed(seed, slot,
// "creative.cutoff"/"creative.pitchDelay", stream type)); the cutoff q is slewed (10 ms) by the
// creative filter. d = 0 gives exactly the static base cutoff and a pure integer pitch delay, so the
// output is bit-identical for every seed. Analog Noise uses its own seeded xoshiro256** stream.
// Graphs: attachGraphInstaller(); at each callback boundary with no fade running, the newest
// published graph is installed and the wet circuit path crossfades old->new over 20 ms (linear,
// equal-sum). The dry path is taken from the old path until the fade ends (the new path's delay
// lines are full by then because latency <= fade length). Latency must be identical. After the
// fade the engine raises the installer's releasedBelow so the worker retires the old processor.
// Graph processors carry no supply/tempco/creative stages: attaching an installer to an engine
// whose circuit has a supply is refused (documented limitation).
// Snapshots: provider layout kSnapshotLayoutId/kSnapshotLayoutVersion (2: macro runtime now holds
// creative motion, noise, detector and creative-filter state; circuit adds rail/feedback state);
// byteSize depends on the active path. Contents: header (sample counter, epoch), macro runtime,
// circuit state, output stage state. attachSnapshotExchange() publishes a capture at the end of the
// call that sees a request (deferred while a graph fade or quality transition runs). With the pitch-
// drift delay enabled capture is refused (cd::control::PitchDriftDelay exposes no state API).
// restoreSnapshot() restores only with an identical SnapshotIdentity (layout, Fs, OS, model hash)
// and a validating payload; otherwise ResetWarm (configuration-only warm reset).
// Threading: process*/reset/tryInstallPrepared/requestQuality/takeRetired/captureState/loadState/
// copyStateFrom/dropGraphs are noexcept with no allocation/locks/I/O/logging; everything else is
// non-audio.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include "blueprint/Timeline.h"
#include "control/CreativeMotion.h"
#include "control/Dynamics.h"
#include "control/Macros.h"
#include "core/ParameterRegistry.h"
#include "dsp/Oversampling.h"
#include "runtime/CircuitRuntime.h"
#include "runtime/OutputStage.h"
#include "runtime/ProcessorGraph.h"
#include "state/SnapshotExchange.h"
#include "state/TransactionQueue.h"

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
    QualityMode mode = QualityMode::Studio;
    // Creative pitch-drift delay: active only when requested and not Zero Latency.
    bool pitchDelay = false, pitchDelayUnavailable = false;
    int pitchLatency = 0;            // declared integer samples (round(1.5 ms * Fs))
    double pitchResidual = 0;        // declared - exact base delay (samples; 0 at 48/96/192 kHz)
};

// Human-readable label for UI/diagnostics, e.g. "Studio 4x FIR", "Manual 2x FIR (Draft)",
// "Zero Latency 1x (Draft)". Non-audio (allocates).
std::string qualityLabel(const QualityInfo& q);

// Macro bindings of the built-in synthetic fixtures: Age (creative shelf) and Warmth (pre-stage
// drive). Sag/Drift are derived by the engine from the circuit's supply / tempco / creative stages.
control::ManifestBindings syntheticMacroBindings() noexcept;

// Creative stages (labelled creative, not physical).
struct CreativeConfig {
    bool cutoffMotion = false;       // creative 2nd-order Butterworth low-pass with Drift motion
    double cutoffHz = 8000;          // base cutoff (20 Hz .. 0.45 Fs)
    bool pitchDelay = false;         // cd::control::PitchDriftDelay (prepare-time; latency)
};

// Initial detector settings (host IDs 1022-1025 change them at runtime).
struct DynamicsConfig {
    double transientPreserve = 0;    // amount 0..1
    LoadSource sidechainSource = LoadSource::Main;
    double sidechainDepth = 0;       // 0..2
    double sidechainHpfHz = 40;      // 20..300
};

struct EngineConfig {
    double sampleRate = 48000;
    int maxBlock = 512;
    int channels = 2;
    QualitySettings quality{};
    // Netlist, synthetic I/O, thermal contract and optional reduced supply. sampleRate/maxBlock/
    // channels/os/maxNewton/padToFamilyMax are overwritten from the fields above.
    CircuitConfig circuit = syntheticDiodeClipper();
    OutputParams initial{};
    int maxNewtonOverride = 0;       // 0 = from quality; 1..12 for diagnostics/tests
    // Macro/base/lifecycle wiring. Bindings describe what the circuit can consume (no guessed units).
    control::ManifestBindings macroBindings = syntheticMacroBindings();
    control::BaseValues base{};                  // stored base physical controls (read-only to macros)
    control::MacroValues initialMacros{};        // normalized a,s,d,w
    double initialLifecycle = 0;                 // slot lifecycle u in [0,1]
    blueprint::Scenario initialScenario = blueprint::Scenario::Default;
    core::ParamScope slotRole = core::ParamScope::SlotA;  // which slot.<x>.* IDs this engine accepts
    CreativeConfig creative{};
    DynamicsConfig dynamics{};
    bool analogNoise = false;                    // initial analogNoise.enabled
    std::uint64_t seed = 0;                      // instance variation seed (creative motion, noise)
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
    // External sidechain input bus (detector only, never audio): 1 or 2 channels; null/0 = absent.
    int sideChannels = 0;
    const double* const* side = nullptr;
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
                   kResetRequested = 1u << 6, kEventsCoalesced = 1u << 7, kGraphFading = 1u << 8,
                   kGraphInstalled = 1u << 9, kSidechainMissing = 1u << 10, kQualityTransition = 1u << 11,
                   kLatencyChangePending = 1u << 12;
}

struct ProcessResult {
    uint32_t frames = 0;
    uint32_t status = status::kOk;
};

enum class QualityRequest : uint8_t { Crossfading, Staged, Rejected };

struct QualityStatus {
    QualityInfo active{}, incoming{}, pending{};
    bool transitioning = false;      // incoming path present (pre-roll or fade)
    bool fading = false;             // audible crossfade running
    bool latencyChangePending = false;
    int warmupLeft = 0, fadePos = 0, fadeLen = 0;
    std::int64_t crossfades = 0, completed = 0, staged = 0, rejected = 0, activations = 0;
    std::int64_t qualityRequests = 0, pitchDelayRequests = 0;   // host IDs 1012-1016 (not applied)
};

struct EngineDiagnostics {
    int64_t samples = 0;
    uint64_t epoch = 0, generation = 0;
    std::int64_t epochChanges = 0, inputNonFinite = 0, inputClamped = 0, eventsCoalesced = 0, staleInstalls = 0;
    // transaction wiring
    std::int64_t paramEvents = 0, unsupportedEvents = 0, unknownEvents = 0, otherSlotEvents = 0, configEvents = 0, noteEvents = 0;
    // graph wiring
    uint64_t graphGeneration = 0;          // installed graph generation (0 = built-in circuit)
    std::int64_t graphInstalls = 0, graphRejected = 0;
    bool graphFading = false;
    int graphFadePos = 0, graphFadeLen = 0;
    // snapshots
    std::int64_t snapshotsPublished = 0, snapshotsDeferred = 0, snapshotsRefused = 0;
    // macros: smoothed values and the last evaluated effective values (base is never modified)
    control::MacroValues macros{};
    control::Effective effective{};
    control::BaseValues base{};
    double driveGain = 1, lifecycle = 0;
    // detectors / creative stages (last processed sample)
    double transientEvidence = 0, preservation = 0, extCurrentA = 0, preserveAmount = 0, sidechainDepth = 0;
    LoadSource sidechainSource = LoadSource::Main;
    bool sidechainConnected = false;
    double cutoffMotionQ = 0, delayMotionQ = 0, creativeCutoffHz = 0, noiseRms = 0, pitchDelaySamples = 0;
    bool analogNoise = false;
    CircuitRuntime::Diagnostics circuit{};
    OutputStage::Diagnostics output{};
};

// Per-prepared-engine macro/lifecycle/creative runtime (trivially copyable: snapshot layout).
struct MacroRuntime {
    control::MacroSmoother smoother;       // 0 age, 1 sag, 2 drift, 3 warmth
    control::MacroSmoother dyn;            // 0 transient.preserve amount, 1 sidechain.depth
    blueprint::LifecycleTimeline timeline;
    double last[6] = {0, 0, 0, 0, 0, 0};   // a, s, d, w, u, q last evaluated
    double targets[4] = {0, 0, 0, 0};
    double dynTargets[2] = {0, 0};
    double drive = 1;
    control::BiquadCoeffs shelf{};
    double z[2][2] = {{0, 0}, {0, 0}};     // shelf TDF-II state per channel
    control::Effective eff{};
    std::int64_t analogNoise = 0;          // analogNoise.enabled
    // creative motion / noise / creative filter
    control::CreativeMotion cutMotion, delayMotion;
    control::Xoshiro256ss noiseRng;
    double noiseGain = 0;                  // linear RMS of the Analog Noise (when enabled)
    double cutOct = 0;                     // slewed cutoff motion [octaves]
    double lpfOct = 0;                     // octave offset of the current creative LPF coefficients
    control::BiquadCoeffs lpf{};
    double lz[2][2] = {{0, 0}, {0, 0}};    // creative LPF TDF-II state per channel
    // detectors
    control::TransientDetector transient;
    control::SidechainLoadDetector sidechain;
    std::int64_t scSource = 0;             // LoadSource
    double scHpfHz = 40;
    double lastB = 0, lastP = 0, lastIExt = 0;
    std::int64_t scConnected = 0;
};

// A fully prepared engine instance (circuit + output stage + slice buffers + macro runtime).
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
    dsp::Oversampler::Spec os_{};
    bool pad_ = true;
    double fs_ = 0;
    int channels_ = 0, maxBlock_ = 0;
    uint64_t generation_ = 0;
    std::vector<double> raw_, gained_, wet_, dry_, wet2_, dry2_, drive_, xout_;
    std::vector<double> shelf_;            // 5 * maxBlock per-sample coefficients (b0,b1,b2,a1,a2)
    std::vector<double> lpfc_;             // 5 * maxBlock creative LPF coefficients
    std::vector<double> cEff_, sag_, pres_, iext_, sens_, dsm_, qd_, ng_;   // per-sample controls
    std::uint64_t cutSeed_ = 0, delaySeed_ = 0, noiseSeed_ = 0;
    MacroRuntime macro_{};
    control::ManifestBindings bindings_{};
    control::BaseValues base_{};
    int fadeLen_ = 1;
    std::uint64_t configHash_ = 0;         // hash of the non-circuit engine configuration
    // creative stages
    bool cutoffOn_ = false, pitchOn_ = false;
    double cutoffHz_ = 8000, cutSlewA_ = 0;
    control::PitchDriftDelay pitch_[2];
    DelayLine pitchDry_[2];
    bool supplyOn_ = false;
    double iBudget_ = 0;
};

class Engine {
public:
    static constexpr uint32_t kMaxEventsPerCall = 4096;
    static constexpr double kEntryLimit = 1e6;   // entry range guard (+120 dBFS), keeps state finite
    static constexpr double kGraphFadeSeconds = 0.020;
    static constexpr double kQualityFadeSeconds = 0.020;
    static constexpr double kCutoffSlewSeconds = 0.010;

    // Non-audio. Returns nullptr with error on failure; active engine unaffected.
    std::unique_ptr<PreparedEngine> buildPrepared(const EngineConfig& cfg, std::string& error);
    // Audio-safe swap. Rejects null or stale (generation <= active) handles, leaving them intact.
    // On success the handle holds the previous engine (possibly null) for off-audio reclamation.
    bool tryInstallPrepared(std::unique_ptr<PreparedEngine>& handle) noexcept;
    // Non-audio convenience: build + install + destroy the previous engine.
    bool prepare(const EngineConfig& cfg, std::string& error);

    ProcessResult process(const ProcessBlock& block) noexcept;
    // Transaction wiring: events sorted by sampleOffset (as drained); offsets relative to this call.
    ProcessResult process(const ProcessContext& ctx, const AudioBuses& buses,
                          std::span<const state::TransactionEvent> events) noexcept;
    // Drains `queue` once (frames = ctx.frames) and processes. Audio thread = queue consumer.
    ProcessResult processQueued(const ProcessContext& ctx, const AudioBuses& buses, state::TransactionQueue& queue) noexcept;
    void reset() noexcept;
    void setSlotRole(core::ParamScope role) noexcept { role_ = role; }
    core::ParamScope slotRole() const noexcept { return role_; }

    bool prepared() const noexcept { return active_ != nullptr; }
    int latencySamples() const noexcept { return active_ ? active_->quality_.latencySamples : 0; }
    QualityInfo quality() const noexcept { return active_ ? active_->quality_ : QualityInfo{}; }
    EngineDiagnostics diagnostics() const noexcept;
    const PreparedEngine* active() const noexcept { return active_.get(); }

    static bool resolveQuality(const QualitySettings& q, int maxNewtonOverride, dsp::Oversampler::Spec& spec,
                               QualityInfo& info) noexcept;

    // ---- live quality transitions (see header) ----
    // Audio-safe, call at a callback boundary (between process calls). Crossfading/Staged take
    // ownership of the handle (Staged returns a previously pending engine through it for off-audio
    // reclamation); Rejected leaves it untouched.
    QualityRequest requestQuality(std::unique_ptr<PreparedEngine>& handle) noexcept;
    // Audio-safe: hands a finished old path (or nothing) to the caller for off-audio reclamation.
    bool takeRetired(std::unique_ptr<PreparedEngine>& out) noexcept;
    bool latencyChangePending() const noexcept { return pending_ != nullptr; }
    int pendingLatencySamples() const noexcept { return pending_ ? pending_->quality_.latencySamples : 0; }
    QualityStatus qualityStatus() const noexcept;
    // Non-audio, audio stopped (host deactivate/reactivate): installs the pending engine, carrying
    // over macro runtime and slow physical state where layouts match. False when nothing is pending.
    bool activatePending(std::string& error);

    // ---- graphs ----
    GraphTarget graphTarget() const noexcept;                     // non-audio, after prepare
    // Non-audio. Refuses an installer whose target does not match the active engine (or an engine
    // with a supply reservoir). Null detaches and returns to the built-in circuit.
    bool attachGraphInstaller(GraphInstaller* installer) noexcept;
    // Audio-safe: return to the built-in circuit and release every graph processor.
    void dropGraphs() noexcept;

    // ---- snapshots ----
    static constexpr std::uint32_t kSnapshotLayoutId = 0x31454443u;  // 'CDE1', provider cd.runtime.Engine
    static constexpr std::uint32_t kSnapshotLayoutVersion = 2;
    state::SnapshotIdentity snapshotIdentity() const noexcept;    // of the active prepared path
    std::size_t snapshotBytes() const noexcept;
    bool attachSnapshotExchange(state::SnapshotExchange* exchange) noexcept;  // non-audio; identity must match
    // Audio-safe. Returns bytes written, 0 when out.size() != snapshotBytes(), nothing prepared, a
    // fade/transition runs or the pitch-drift delay is enabled.
    std::size_t captureState(std::span<std::byte> out) const noexcept;
    // Audio-safe. Validates the whole payload first; applies only if valid (all or nothing).
    bool loadState(std::span<const std::byte> in) noexcept;
    // Non-audio, while audio is stopped (like prepare). Restore only on identical identity and a
    // validating payload; otherwise reset() and ResetWarm (NoCheckpoint when nothing saved).
    state::RestoreDecision restoreSnapshot(const state::SavedSnapshot& saved, std::string& reason);
    // A/B copy (audio-safe): copies src's complete dynamic state and parameter targets when the
    // snapshot identities match; scratch must hold snapshotBytes(). False leaves *this unchanged.
    bool copyStateFrom(const Engine& src, std::span<std::byte> scratch) noexcept;

private:
    void applyEvent(PreparedEngine& p, const ParamEvent& e) noexcept;
    void applyTransaction(PreparedEngine& p, const state::TransactionEvent& e, bool count) noexcept;
    void applyHostParam(PreparedEngine& p, uint32_t hostId, double normalized, bool count) noexcept;
    void applyEverywhere(const state::TransactionEvent& e) noexcept;
    void applyEverywhere(const ParamEvent& e) noexcept;
    void runSlice(PreparedEngine& p, const AudioBuses& buses, int offset, int n, uint32_t& st) noexcept;
    void runPath(PreparedEngine& p, const double* const* in, const AudioBuses& buses, int offset, double* const* out, int n,
                 uint32_t& st, bool primary) noexcept;
    void macroSlice(PreparedEngine& p, const AudioBuses& buses, int offset, int n, uint32_t& st) noexcept;
    void wetStages(PreparedEngine& p, double* const* wet, double* const* dry, int n) noexcept;
    void graphBoundary(PreparedEngine& p, uint32_t& st) noexcept;
    void captureBoundary() noexcept;
    bool validateCall(const ProcessContext& ctx, const AudioBuses& buses, ProcessResult& r) noexcept;
    uint32_t finishStatus(PreparedEngine& p, uint32_t st) noexcept;
    CircuitRuntime& currentCircuit(PreparedEngine& p) noexcept;
    const CircuitRuntime& currentCircuit() const noexcept;
    void writeState(StateWriter& w) const noexcept;
    bool readState(StateReader& r, bool apply) noexcept;
    void finishQualityFade() noexcept;

    std::unique_ptr<PreparedEngine> active_;
    uint64_t nextGeneration_ = 0;
    int64_t samples_ = 0;
    uint64_t epoch_ = 0;
    bool haveEpoch_ = false;
    std::int64_t epochChanges_ = 0, inputNonFinite_ = 0, inputClamped_ = 0, eventsCoalesced_ = 0, staleInstalls_ = 0;
    core::ParamScope role_ = core::ParamScope::SlotA;
    // graphs (audio-side pointers; ownership stays in the installer's pool)
    GraphInstaller* graphs_ = nullptr;
    GraphProcessor* curGraph_ = nullptr;   // null = built-in circuit of the PreparedEngine
    GraphProcessor* oldGraph_ = nullptr;   // fading-out graph path (null + fading_ = built-in fading out)
    bool fading_ = false;
    int fadePos_ = 0;
    std::int64_t graphInstalls_ = 0, graphRejected_ = 0;
    // live quality transitions
    std::unique_ptr<PreparedEngine> incoming_, retired_, pending_;
    int qWarm_ = 0, qPos_ = 0, qLen_ = 1;
    std::int64_t qCrossfades_ = 0, qCompleted_ = 0, qStaged_ = 0, qRejected_ = 0, qActivations_ = 0;
    std::int64_t qRequests_ = 0, pitchRequests_ = 0;
    // snapshots
    state::SnapshotExchange* snap_ = nullptr;
    std::int64_t snapPublished_ = 0, snapDeferred_ = 0, snapRefused_ = 0;
    // transaction counters
    std::int64_t paramEvents_ = 0, unsupported_ = 0, unknown_ = 0, otherSlot_ = 0, configEvents_ = 0, noteEvents_ = 0;
};

}  // namespace cd::runtime
