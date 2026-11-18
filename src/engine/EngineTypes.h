// CircuitDrift engine: public framework-independent types (CD-F20..F25, docs/ARCHITECTURE.md).
// Every circuit value here is a SYNTHETIC FIXTURE (G01 open): round illustrative numbers, not
// calibrated hardware. POD types only; safe to copy on the audio thread.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cd::engine {

constexpr int kSlots = 2, kMaxChannels = 2;

struct EngineConfig {
    double sampleRate = 48000;
    int maxBlock = 512;       // largest host block; larger calls are split internally
    int channels = 2;         // 1 or 2
    uint64_t seed = 0x5EEDC0DEull;           // instance seed (noise streams, see Rng::seed)
    // Stable per-slot component-variation seeds (tolerance draws). Equal by default so identical
    // slot settings give identical circuits (T24 identical-slot unity); a state layer may set them.
    uint64_t variationSeed[kSlots] = {0x5EEDC0DEull, 0x5EEDC0DEull};
};

struct Transport { bool playing = false; double bpm = 0; double ppq = 0; bool ppqValid = false; };

struct ProcessContext {
    double sampleRate = 48000;  // must equal the prepared rate (mismatch is flagged, prepared rate used)
    int64_t sampleStart = 0;
    uint64_t epoch = 0;         // a change = transport seek: creative pitch/delay state re-centres
    uint32_t frames = 0;
    bool offline = false;
    Transport transport{};
};

enum class ResetKind : uint8_t { Warm, Cold };

// Active element families (slot.*.stage.activeElement order).
enum class Family : uint8_t { Triode12AX7 = 0, PentodeEF86 = 1, Bjt = 2, Jfet = 3 };
constexpr int kFamilyCount = 4;

// ---- Component identities ----------------------------------------------------------------------
// Stable IDs (inspector, tooltips, faults, timeline, blueprints). Never renumber; append only.
enum class ComponentId : uint8_t {
    C_in, R_g, V1, R_load, R_k, C_k, C_out, T_out, C_dec, R_dec, R_g2, C_g2, C_res, D_rect, R_filt, C_filt, Chassis, Count
};
constexpr int kComponentCount = int(ComponentId::Count);

enum class ComponentRole : uint8_t {
    InputCoupling, GridLeakBias, ActiveElement, PlateLoad, CathodeResistor, CathodeBypass, OutputCoupling,
    OutputTransformer, Decoupling, DecouplingResistor, ScreenResistor, ScreenBypass, Reservoir, Rectifier,
    FilterResistor, FilterCapacitor, Chassis
};
enum class ComponentScope : uint8_t { PerChannel, PerSlot };

struct ComponentInfo {
    ComponentId id;
    const char* key;          // stable string ID, e.g. "C_res"
    ComponentRole role;
    ComponentScope scope;
    const char* displayName;
    const char* material;     // material/technology description
    const char* units;        // primary value unit
    const char* modelId;      // device manifest (synthetic catalogue) or reduced-model name
    const char* provenance;   // always "synthetic-fixture, not calibrated hardware (G01)" + notes
};
const ComponentInfo* componentTable() noexcept;  // kComponentCount entries, indexed by ComponentId
const ComponentInfo* findComponent(const char* key) noexcept;

// Per-component condition overlay (timeline CD-F18 / faults CD-F19 producers). Identity default
// changes nothing. All fields are bounded on use: valueScale [0.05,20], leakage [0,1] fraction of
// the component's safe leakage span, esrScale [1,1000], noiseScale [0,100]; `open` drives the
// open-equivalent (C to manifest minimum + high series R / R to open-equivalent), `shortish`
// lowers leakage/series resistance to its short-equivalent bound (never ideal 0 or infinity).
struct ComponentCondition {
    double valueScale = 1.0;
    double leakage = 0.0;
    double esrScale = 1.0;
    double noiseScale = 1.0;
    bool open = false;
    bool shortish = false;
};
struct ConditionSet {
    ComponentCondition c[kComponentCount];
};

// ---- Effective block targets (macro/condition evaluation result; inspectable, T06 endpoints) ----
struct EffectiveTargets {
    double driveDb = 0, biasNormalized = 0, biasVolts = 0;
    double ageYears = 0, toleranceSigma = 0, leakageFraction = 0;
    double reservoirCapF = 0, reservoirCapBaseF = 0;
    double sagAugmentation = 0;     // 3*s (Main/Blend), 0 for External
    int sidechainMode = 0;
    double fluxDriveFactor = 1, saturationFluxWb = 0;
    double tempcoScale = 1;         // driftCoefficient * thermalSensitivityScale
    double hfShelfDb = 0, creativeCutoffOct = 0, creativeDelayMs = 0;
    bool creativeDelayActive = false;
    double recoveryTauS = 0, recoveryConductanceScale = 1;
    double ageNoiseDbfs = -200;
    bool ageNoiseGated = true;      // true when analogNoise.enabled is off (age noise not added)
    double thermalMassScale = 1, ambientK = 298.15;
    double mainsVolts = 117, mainsHz = 50;
    int family = 0, rectifier = 0, core = 0, resistorType = 0;
};

// ---- Telemetry (audio thread writes, UI reads; triple buffered, never blocks audio) -------------
constexpr int kRingSize = 256;
struct SlotTelemetry {
    double temperatureK[kMaxChannels][kComponentCount] = {};  // per-slot components repeat in both channels
    double powerW[kMaxChannels][kComponentCount] = {};
    double railVolts = 0, railDcVolts = 0, railNominalVolts = 0;
    double stageCurrentA[kMaxChannels] = {};
    double fluxWb[kMaxChannels] = {}, fieldH[kMaxChannels] = {}, fluxDensityT[kMaxChannels] = {};
    int family = 0, transitionFamily = -1;
    double railRing[kRingSize] = {};             // V_res every ringDecimation host samples
    double bhRing[kRingSize][2] = {};            // channel 0 (H [A/m], B [T]) pairs, same cadence
};
struct TelemetryFrame {
    uint64_t generation = 0;
    int64_t sampleIndex = 0;
    double sampleRate = 0;
    int latencySamples = 0;
    // Active vs pending quality (pending differs only while a staged change awaits re-prepare).
    int activeFactor = 0, activeFamilyIir = 0, activeNewtonCap = 0;
    bool activeZeroLatency = false, activePitchDrift = false;
    int pendingFactor = 0, pendingFamilyIir = 0;
    bool pendingZeroLatency = false, pendingPitchDrift = false, reprepareRequired = false, qualityTransition = false;
    // Levels over the last published block.
    double inPeak[kMaxChannels] = {}, inRms[kMaxChannels] = {}, outPeak[kMaxChannels] = {}, outRms[kMaxChannels] = {};
    // Flags / counters.
    bool solverFallback = false, needsReset = false, bypassed = false, hpfEnabled = true, softClipEnabled = true;
    long solverFailures = 0, nonFiniteOutputs = 0, clampedOutputs = 0, nonFiniteInputs = 0;
    int ringDecimation = 1, ringWrite = 0;       // index of the next ring write (oldest entry)
    double gainMatch = 1;
    SlotTelemetry slot[kSlots];
    double transferRing[kRingSize][2] = {};      // (aligned dry, wet) channel 0, host rate
    int transferWrite = 0;
};

// ---- Checkpoint (exact continuation of the dynamic state; T24 part) ---------------------------
struct Checkpoint {
    uint64_t layout = 0;             // configuration hash; setCheckpoint refuses mismatches
    int64_t sampleIndex = 0;
    std::vector<double> values;      // every double state (incl. integer positions as doubles)
    std::vector<uint64_t> rng;       // RNG states
};

}  // namespace cd::engine
