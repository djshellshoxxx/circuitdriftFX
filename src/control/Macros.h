// Macro evaluator (P05, spec 02-musical-controls "One-way parameter composition", D03/D09).
// Owner: cd_control. Threading: pure functions and a per-sample smoother; audio-thread safe
// (noexcept, no allocation). Effective values are NEVER written back to base: evaluate takes const base.
// Order: base -> timeline offsets -> macros -> modulation -> faults -> domain clamp.
// The Age shelf is a creative tilt, not a capacitor-aging equation.
#pragma once
#include <array>
#include <cstddef>

namespace cd::control {

struct MacroValues { double age = 0, sag = 0, drift = 0, warmth = 0; };  // a,s,d,w in [0,1]

// Model-manifest bindings. A false flag means the manifest lacks that binding: macro unsupported, neutral.
struct ManifestBindings {
    bool hasAge = false, hasSag = false, hasWarmth = false, hasDrift = false;
    bool analogNoise = false;          // Age noise only when Analog Noise is enabled
    double cBase = 0, cMin = 0;        // farads (sag)
    double biasSpan = 0;               // allowed bias span (manifest units)
    double leakRange = 1;              // declared safe leakage range (fraction scale)
    double maxTolSigma = 0.25;         // global +-25% envelope
    double fluxMin = 1.0, fluxMax = 1.5;  // safe transformer flux-factor domain (applied when warmth bound)
};

// Base (stored) physical controls. Neutral defaults.
struct BaseValues {
    double serviceAgeYears = 0, tolSigma = 0, leakFrac = 0;
    double driveDb = 0, biasOffset = 0, fluxFactor = 1;
    double driftSensitivity = 1, loadMultiplier = 1;
};

struct TimelineOffsets { double serviceAgeYears = 0, tolSigma = 0, leakFrac = 0, driveDb = 0, biasOffset = 0; };
struct ModulationInput { double q = 0; };  // creative motion in [-1,1] (CreativeMotion::q)
struct FaultOverlay { double leakFracAdd = 0, loadMultiplierScale = 1, biasOffsetAdd = 0; };

struct Effective {
    double serviceAgeYears = 0, tolSigma = 0, leakFrac = 0;
    double shelfGainDb = 0, shelfHz = 6000, shelfQ = 0.707;
    bool noiseEnabled = false; double noiseDbfs = 0;   // valid only if noiseEnabled
    double reservoirC = 0, loadMultiplier = 1;
    double driveDb = 0, biasOffset = 0, fluxFactor = 1;
    double driftSensitivity = 1, cutoffMotionOct = 0, delayDevMs = 0;
    bool supportsAge = false, supportsSag = false, supportsWarmth = false, supportsDrift = false;
};

// Optional hook inputs; nullptr = identity.
struct Hooks { const TimelineOffsets* timeline = nullptr; const ModulationInput* mod = nullptr; const FaultOverlay* fault = nullptr; };

Effective evaluate(const BaseValues& base, const MacroValues& m, const ManifestBindings& mb,
                   const Hooks& hooks = {}) noexcept;

// Sidechain selector precedence (spec): Main I_phys+3 s I_dyn; External I_phys+I_ext; Blend both.
enum class SidechainSource { Main, External, Blend };
double composeLoadCurrent(SidechainSource src, double iPhys, double iDyn, double sag, double iExt) noexcept;

// RBJ Audio EQ Cookbook high shelf, normalized (a0 = 1), TDF-II. Invalid args -> identity.
struct BiquadCoeffs { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };
BiquadCoeffs designHighShelf(double fs, double f0, double gainDb, double q) noexcept;

// 20 ms one-pole smoothing per continuous control; per-sample stepping makes output block-partition invariant.
class MacroSmoother {
public:
    static constexpr std::size_t kMax = 16;
    void prepare(double sampleRate, const double* initial, std::size_t n, double tauSeconds = 0.020) noexcept;
    void setTargets(const double* t, std::size_t n) noexcept;  // non-finite entries ignored
    void step() noexcept;                                      // advance one sample
    double value(std::size_t i) const noexcept { return i < kMax ? y_[i] : 0.0; }
    std::size_t size() const noexcept { return n_; }
private:
    std::array<double, kMax> y_{}, t_{};
    double a_ = 0; std::size_t n_ = 0;
};

}  // namespace cd::control
