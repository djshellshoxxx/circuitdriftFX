// CircuitDrift runtime: integrated engine (P03 integration slice + resume packet 1 wiring).
// Composition per host sample: timestamped events -> entry sanitize/range guard -> input gain ->
// macro/lifecycle effective values -> CircuitRuntime (oversampled MNA, thermal clock, latched
// solver fallback; built-in circuit or crossfaded blueprint graph paths) -> Age shelf on wet ->
// OutputStage (level-matched blend -> trim -> HPF -> oversampled soft clip -> host bypass ->
// containment).
// Owner: cd_runtime. Depends on cd_core/cd_dsp/cd_control/cd_state/cd_blueprint; never on UI/JUCE.
//
// Lifetime/generations: buildPrepared() (non-audio) builds a complete candidate with a new
// generation number; tryInstallPrepared() (audio-safe, noexcept) swaps it in only if its generation
// is newer than the active one and hands the previous engine back through the same handle so the
// caller reclaims it off the audio thread. prepare() = build + install + reclaim, for use while
// audio is stopped. A failed build leaves the active engine untouched. Installing a prepared
// engine whose configuration no longer matches the active graph path returns to the built-in
// circuit (graph processors are released, never destroyed here).
// Quality is fixed per prepared engine; live quality transitions with the 20 ms crossfade and the
// host latency-change handshake are NOT implemented (later work). Zero Latency is a separate
// preparation mode (1x, no padding, native-rate soft clip).
// Clocks: the sample counter advances only by processed frames; a zero-frame call applies events
// only. A changed ProcessContext::epoch is counted (no network/pitch history exists yet to clear);
// capacitor/thermal state persists across epochs.
// Unsupported layouts/rates are explicit: channels 1|2 (in == out count), 44.1-192 kHz, double
// samples only (32-bit host conversion belongs to adapters). A context whose sampleRate or channel
// count differs from the prepared engine yields silence and a status flag; clocks do not advance.
//
// Transaction events: process(ctx, buses, events) / processQueued(ctx, buses, queue) apply
// cd::state::TransactionEvent Params at their sampleOffset (the call is split at every event
// offset and at maxBlock). The queue is drained ONCE per process call with frames = call frames,
// so queue offsets are relative to the call (the call is the queue's consuming slice). Host IDs
// map through the cd::core registry (toPlain) to: macro.age/sag/drift/warmth, input.gain,
// output.gain (trim), mix, gainMatch.enabled (auto-gain), output.softClip, hpf.enabled,
// hpf.frequency, slot.<role>.lifecycle and slot.<role>.scenario. Other registry IDs are counted as
// unsupported, unregistered IDs as unknown, the other A/B slot's IDs as otherSlot; none change
// audio. ConfigPublish events are counted only: graph installs are driven by the GraphInstaller
// pool (newest publish wins). Note events are counted (no voices exist).
// Macros: per-sample cd::control::MacroSmoother (20 ms) feeding cd::control::evaluate(base,
// macros, bindings, timeline offsets) whenever a smoothed value changes; effective values are never
// written back to base or to host parameters. What the SYNTHETIC circuits consume
// (EngineConfig::macroBindings): Warmth -> pre-stage drive dB on the circuit input (wet only); Age
// -> RBJ high shelf -6*a dB at 6 kHz, Q 0.707 on the wet path (creative tilt). Sag, Drift and
// Age's service-age/tolerance/leakage and Warmth's bias/flux have no binding in the synthetic
// fixtures (no supply reservoir, no temperature law, no creative pitch delay): Sag/Drift report
// unsupported and are neutral. Lifecycle: slot lifecycle/scenario drive a
// cd::blueprint::LifecycleTimeline whose condition becomes evaluate()'s timeline offsets; the
// synthetic circuits have no consumer for those effective values (telemetry only).
// Graphs: attachGraphInstaller(); at each callback boundary with no fade running, the newest
// published graph is installed and the wet circuit path crossfades old->new over 20 ms (linear,
// equal-sum). The dry path is taken from the old path until the fade ends (the new path's delay
// lines are full by then because latency <= fade length). Latency must be identical. After the
// fade the engine raises the installer's releasedBelow so the worker retires the old processor.
// Snapshots: provider layout kSnapshotLayoutId/kSnapshotLayoutVersion; byteSize depends on the
// active path. Contents: header (sample counter, epoch), macro runtime, circuit state (MnaSolver
// getState unknowns+charges, oversampler histories, latency pads, dry delay, thermal temperatures/
// energy, 1 kHz clock accumulator, fallback latch/fade, counters), output stage state.
// attachSnapshotExchange() publishes a capture at the end of the call that sees a request
// (deferred while a graph fade runs). restoreSnapshot() restores only with an identical
// SnapshotIdentity (layout, Fs, OS, model hash) and a validating payload; otherwise ResetWarm
// (configuration-only: warm reset of the prepared configuration).
// Threading: process*/reset/tryInstallPrepared/captureState/loadState/copyStateFrom/dropGraphs are
// noexcept with no allocation/locks/I/O/logging; everything else is non-audio.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include "blueprint/Timeline.h"
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
};

// Macro bindings of the built-in synthetic fixtures: Age (creative shelf) and Warmth (pre-stage
// drive) only. No supply reservoir, bias span, flux domain or temperature law exists (G01/G02).
control::ManifestBindings syntheticMacroBindings() noexcept;

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
    // Macro/base/lifecycle wiring. Bindings describe what the circuit can consume (no guessed units).
    control::ManifestBindings macroBindings = syntheticMacroBindings();
    control::BaseValues base{};                  // stored base physical controls (read-only to macros)
    control::MacroValues initialMacros{};        // normalized a,s,d,w
    double initialLifecycle = 0;                 // slot lifecycle u in [0,1]
    blueprint::Scenario initialScenario = blueprint::Scenario::Default;
    core::ParamScope slotRole = core::ParamScope::SlotA;  // which slot.<x>.* IDs this engine accepts
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
                   kResetRequested = 1u << 6, kEventsCoalesced = 1u << 7, kGraphFading = 1u << 8,
                   kGraphInstalled = 1u << 9;
}

struct ProcessResult {
    uint32_t frames = 0;
    uint32_t status = status::kOk;
};

struct EngineDiagnostics {
    int64_t samples = 0;
    uint64_t epoch = 0, generation = 0;
    long epochChanges = 0, inputNonFinite = 0, inputClamped = 0, eventsCoalesced = 0, staleInstalls = 0;
    // transaction wiring
    long paramEvents = 0, unsupportedEvents = 0, unknownEvents = 0, otherSlotEvents = 0, configEvents = 0, noteEvents = 0;
    // graph wiring
    uint64_t graphGeneration = 0;          // installed graph generation (0 = built-in circuit)
    long graphInstalls = 0, graphRejected = 0;
    bool graphFading = false;
    int graphFadePos = 0, graphFadeLen = 0;
    // snapshots
    long snapshotsPublished = 0, snapshotsDeferred = 0, snapshotsRefused = 0;
    // macros: smoothed values and the last evaluated effective values (base is never modified)
    control::MacroValues macros{};
    control::Effective effective{};
    control::BaseValues base{};
    double driveGain = 1, lifecycle = 0;
    CircuitRuntime::Diagnostics circuit{};
    OutputStage::Diagnostics output{};
};

// Per-prepared-engine macro/lifecycle runtime (trivially copyable: part of the snapshot layout).
struct MacroRuntime {
    control::MacroSmoother smoother;       // 0 age, 1 sag, 2 drift, 3 warmth
    blueprint::LifecycleTimeline timeline;
    double last[5] = {0, 0, 0, 0, 0};      // a, s, d, w, u last evaluated
    double targets[4] = {0, 0, 0, 0};
    double drive = 1;
    control::BiquadCoeffs shelf{};
    double z[2][2] = {{0, 0}, {0, 0}};     // shelf TDF-II state per channel
    control::Effective eff{};
    std::int64_t analogNoise = 0;          // stored; no noise generator exists (counted unsupported)
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
    std::vector<double> raw_, gained_, wet_, dry_, wet2_, dry2_, drive_;
    std::vector<double> shelf_;            // 5 * maxBlock per-sample coefficients (b0,b1,b2,a1,a2)
    MacroRuntime macro_{};
    control::ManifestBindings bindings_{};
    control::BaseValues base_{};
    int fadeLen_ = 1;
    std::uint64_t configHash_ = 0;         // hash of the non-circuit engine configuration
};

class Engine {
public:
    static constexpr uint32_t kMaxEventsPerCall = 4096;
    static constexpr double kEntryLimit = 1e6;   // entry range guard (+120 dBFS), keeps state finite
    static constexpr double kGraphFadeSeconds = 0.020;

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

    // ---- graphs ----
    GraphTarget graphTarget() const noexcept;                     // non-audio, after prepare
    // Non-audio. Refuses an installer whose target does not match the active engine. Null detaches
    // and returns to the built-in circuit.
    bool attachGraphInstaller(GraphInstaller* installer) noexcept;
    // Audio-safe: return to the built-in circuit and release every graph processor.
    void dropGraphs() noexcept;

    // ---- snapshots ----
    static constexpr std::uint32_t kSnapshotLayoutId = 0x31454443u;  // 'CDE1', provider cd.runtime.Engine
    static constexpr std::uint32_t kSnapshotLayoutVersion = 1;
    state::SnapshotIdentity snapshotIdentity() const noexcept;    // of the active prepared path
    std::size_t snapshotBytes() const noexcept;
    bool attachSnapshotExchange(state::SnapshotExchange* exchange) noexcept;  // non-audio; identity must match
    // Audio-safe. Returns bytes written, 0 when out.size() != snapshotBytes() or nothing prepared.
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
    void applyEvent(const ParamEvent& e) noexcept;
    void applyTransaction(PreparedEngine& p, const state::TransactionEvent& e) noexcept;
    void applyHostParam(PreparedEngine& p, uint32_t hostId, double normalized) noexcept;
    void runSlice(PreparedEngine& p, const double* const* in, double* const* out, int offset, int n, uint32_t& st) noexcept;
    void macroSlice(PreparedEngine& p, int n) noexcept;
    void graphBoundary(PreparedEngine& p, uint32_t& st) noexcept;
    void captureBoundary() noexcept;
    bool validateCall(const ProcessContext& ctx, const AudioBuses& buses, ProcessResult& r) noexcept;
    uint32_t finishStatus(PreparedEngine& p, uint32_t st) noexcept;
    CircuitRuntime& currentCircuit(PreparedEngine& p) noexcept;
    const CircuitRuntime& currentCircuit() const noexcept;
    void writeState(StateWriter& w) const noexcept;
    bool readState(StateReader& r, bool apply) noexcept;

    std::unique_ptr<PreparedEngine> active_;
    uint64_t nextGeneration_ = 0;
    int64_t samples_ = 0;
    uint64_t epoch_ = 0;
    bool haveEpoch_ = false;
    long epochChanges_ = 0, inputNonFinite_ = 0, inputClamped_ = 0, eventsCoalesced_ = 0, staleInstalls_ = 0;
    core::ParamScope role_ = core::ParamScope::SlotA;
    // graphs (audio-side pointers; ownership stays in the installer's pool)
    GraphInstaller* graphs_ = nullptr;
    GraphProcessor* curGraph_ = nullptr;   // null = built-in circuit of the PreparedEngine
    GraphProcessor* oldGraph_ = nullptr;   // fading-out graph path (null + fading_ = built-in fading out)
    bool fading_ = false;
    int fadePos_ = 0;
    long graphInstalls_ = 0, graphRejected_ = 0;
    // snapshots
    state::SnapshotExchange* snap_ = nullptr;
    long snapPublished_ = 0, snapDeferred_ = 0, snapRefused_ = 0;
    // transaction counters
    long paramEvents_ = 0, unsupported_ = 0, unknown_ = 0, otherSlot_ = 0, configEvents_ = 0, noteEvents_ = 0;
};

}  // namespace cd::runtime
