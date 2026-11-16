// TempoClock (CD-F13): beat-division timing, phase and recovery-coefficient derivation.
// Owner module: cd_control. Threading: prepare() any thread before use; setHost()/step()/advance()
// are audio-thread safe (noexcept, no allocation, double state). Per-sample stepping makes results
// independent of block partition provided the host feeds consistent PPQ values.
#pragma once
#include <cstddef>

namespace cd::control {

enum class Division { ThirtySecond, Sixteenth, Eighth, Quarter, Half, Bar };
enum class Modifier { Straight, Dotted, Triplet };

struct HostTempo {
    double bpm = 0.0;
    bool bpmValid = false;
    double ppq = 0.0;  // quarter-note position at the start of the block
    bool ppqValid = false;
    int timeSigNum = 4;
    int timeSigDen = 4;
    bool timeSigValid = false;
    bool transportJump = false;  // seek/loop: force phase reset from ppq
};

struct TempoStatus {
    bool bpmFallback = true;      // 120 BPM in use because host BPM missing/invalid (S05 indicator)
    bool bpmClamped = false;      // host BPM outside 20..300 was clamped
    bool ppqFreeRun = true;       // phase follows the sample clock, not host PPQ
    bool timeSigFallback = true;  // 4/4 default in use
};

class TempoClock {
public:
    static constexpr double kFallbackBpm = 120.0;
    static constexpr double kMinBpm = 20.0;
    static constexpr double kMaxBpm = 300.0;
    static constexpr double kCoefficientSmoothSeconds = 0.050;
    static constexpr double kSeekSmoothingSeconds = 0.020;  // consumer-side audible-state smoothing target
    static constexpr double kPpqResyncTolerance = 0.05;      // quarter notes

    // Beats (quarter notes) for a division. Bars use num*4/den.
    static double beats(Division d, Modifier m, int tsNum, int tsDen) noexcept;
    static double durationSeconds(double beats, double bpm) noexcept;  // D = 60 q / BPM
    static double tauSeconds(double durationSeconds) noexcept;         // D / ln(20)
    // Per-sample one-pole coefficient a so that y += a (x - y) recovers 95% in D.
    static double coefficientFor(double tauSeconds, double sampleRate) noexcept;

    bool prepare(double sampleRate) noexcept;
    void setDivision(Division d, Modifier m) noexcept;
    void setHost(const HostTempo& h) noexcept;
    void step() noexcept;
    void advance(std::size_t samples) noexcept;

    double phase() const noexcept;  // frac(ppq / q) in [0,1)
    double ppq() const noexcept { return ppq_; }
    double bpm() const noexcept { return bpm_; }
    double beatsPerCycle() const noexcept { return q_; }
    double durationSeconds() const noexcept { return d_; }
    double tau() const noexcept { return tau_; }
    double recoveryCoefficient() const noexcept { return coef_; }  // smoothed
    double targetCoefficient() const noexcept { return coefTarget_; }
    const TempoStatus& status() const noexcept { return status_; }
    // Creative sinusoidal drift in [-depth, depth]; depth clamped to 0..1.
    double drift(double depth) const noexcept;
    // Snapshot support: finite, in-range internals (enums, time signature, BPM 20..300 or fallback,
    // coefficients in [0,1], bounded ramp). A restored copy that fails this must be rejected.
    bool validState() const noexcept;

private:
    void retarget() noexcept;
    double fs_ = 0.0;
    Division div_ = Division::Quarter;
    Modifier mod_ = Modifier::Straight;
    int tsNum_ = 4, tsDen_ = 4;
    double bpm_ = kFallbackBpm, q_ = 1.0, d_ = 0.5, tau_ = 0.0;
    double ppq_ = 0.0;
    bool havePpq_ = false;
    double coef_ = 0.0, coefTarget_ = 0.0, coefInc_ = 0.0;
    std::size_t rampLeft_ = 0;
    TempoStatus status_;
};

}  // namespace cd::control
