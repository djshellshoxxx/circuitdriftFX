// CircuitDrift blueprint: component fault injector (CD-F19, 03-blueprints "Fault injector").
// Per-component overlays: ElectrolyticOpen, ElectrolyticLeaky, CrackedResistor (seeded intermittent
// contact), MicrophonicTube (2-pole resonant bias perturbation). A FaultChannel turns
// (overlay, manifest-declared bounds, excitation) into bounded parameter values per electrical
// sample. Values are never ideal 0/inf: every target is clamped inside FaultBounds, which must come
// from the component manifest's declared open/short equivalents. Artistic bounded failure
// simulation, NOT a validated failure model (G01 open); the microphonic mode is a proposed
// mechanical-coupling surrogate, not a measured tube response.
// Threading: prepare()/configure objects on the worker; tick/apply/reset are noexcept and allocation
// free (audio-safe) after prepare.
#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace cd::blueprint {

enum class FaultType : unsigned char { ElectrolyticOpen, ElectrolyticLeaky, CrackedResistor, MicrophonicTube };
constexpr int kFaultTypeCount = 4;
constexpr double kBiasPerturbationMax = 0.05;           // fraction of the safe bias span
constexpr double kValueRampSeconds = 0.010;             // C/R changes: log interpolation over 10 ms
constexpr double kContactRampSeconds = 0.002;           // cracked-resistor contact transitions
constexpr double kResetRampSeconds = 0.020;             // Reset Faults
constexpr double kMinContactDwellSeconds = 0.050;       // => at most 20 contact transitions per second

struct FaultOverlay {
    FaultType type = FaultType::ElectrolyticOpen;
    double severity = 0;                                // 0..1
    bool enabled = false;                               // default off
    std::uint64_t seed = 0;
    std::uint32_t revision = 0;
    bool intermittent = true;                           // CrackedResistor: seeded intermittent contact
    double frequencyHz = 800;                           // MicrophonicTube: 100..4000
    double q = 8;                                       // MicrophonicTube: 2..30
};

// Manifest-declared limits and nominal values of the faulted component (all strictly positive).
struct FaultBounds {
    double nominalC = 1e-6, minC = 1e-9;                // F: manifest minimum capacitance
    double nominalEsr = 0.1, openSeriesR = 1e6;         // ohm: declared open-equivalent series resistance
    double nominalLeakR = 1e9, shortLeakR = 100;        // ohm: declared short-equivalent parallel resistance
    double nominalR = 1e4, openR = 1e8;                 // ohm: carbon resistor nominal / open-equivalent
    double biasSpanV = 1.0;                             // safe bias span (volts)
};

struct FaultOutputs {
    double capacitanceF = 0, seriesROhm = 0, leakageROhm = 0, resistanceOhm = 0;
    double biasPerturbationV = 0;                       // |.| <= 0.05 * severity * biasSpanV
    bool contactOpen = false;                           // CrackedResistor intermittent state
};

bool validOverlay(const FaultOverlay&) noexcept;        // ranges, finite values
bool validBounds(const FaultBounds&) noexcept;
// Availability of a fault for a component: reason is a static string, empty when available.
// Tube faults need a validated tube topology, which does not exist yet (G01), so they report why.
bool faultAvailable(FaultType type, bool componentIsCapacitor, bool componentIsResistor, bool componentIsTube,
                    const char*& reason) noexcept;

class FaultChannel {
public:
    // Worker: validates, sets electrical rate (Fs*OS), measures the microphonic impulse response peak.
    bool prepare(double electricalRate, const FaultBounds& bounds, const FaultOverlay& overlay, std::string& error);
    // Audio-safe. Rejects invalid overlays and stale revisions (older than current); returns accepted.
    bool apply(const FaultOverlay& overlay) noexcept;
    // Reset Faults: 20 ms ramp back to nominal; afterwards the overlay is cleared (disabled,
    // severity 0, resonator state zero). No other state is touched.
    void reset() noexcept;
    // One electrical sample. excitation = main transient detector output (>= 0), clamped to [0,1].
    FaultOutputs tick(double excitation) noexcept;
    std::int64_t contactTransitions() const noexcept { return transitions_; }
    bool cleared() const noexcept { return clearedDone_; }
    double resonatorGain() const noexcept { return g_; }            // normalised g (prepare)
    const FaultOverlay& overlay() const noexcept { return ov_; }
private:
    struct Ramp { double cur = 0, step = 0, target = 0, value = 0, exact = 0; std::int64_t left = 0; };  // cur/target in the ramp's domain
    static void retarget(Ramp& r, double targetValue, bool logDomain, std::int64_t samples) noexcept;
    static void advance(Ramp& r, bool logDomain) noexcept;
    void recomputeTargets(std::int64_t samples) noexcept;
    void scheduleNextContact(bool afterLast) noexcept;
    std::uint64_t nextRandom() noexcept;

    FaultBounds b_;
    FaultOverlay ov_;
    bool prepared_ = false, resetting_ = false, clearedDone_ = true;
    double rate_ = 48000, preparedF_ = 800, preparedQ_ = 8;
    std::int64_t valueRamp_ = 480, contactRamp_ = 96, resetRamp_ = 960, minDwell_ = 2400, resetLeft_ = 0;
    Ramp c_, esr_, leak_, r_, mic_;                                 // c/esr/leak/r: log domain; mic: linear scale 0..severity
    bool contactOpen_ = false;
    std::int64_t nextToggle_ = 0, lastToggle_ = -INT64_C(1000000000), sample_ = 0, transitions_ = 0;
    std::uint64_t rng_ = 0;
    double a1_ = 0, a2_ = 0, g_ = 1, z1_ = 0, z2_ = 0;              // resonator
};

// Fixed bank of channels so Reset Faults clears every overlay together.
class FaultBank {
public:
    static constexpr int kMax = 16;
    FaultChannel& channel(int i) noexcept { return ch_[std::size_t(i)]; }
    void resetAll() noexcept { for (auto& c : ch_) c.reset(); }
private:
    std::array<FaultChannel, kMax> ch_;
};

}  // namespace cd::blueprint
