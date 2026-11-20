// cd_control: tempo breathing (CD-F13). Creative retiming of supply recovery plus sinusoidal creative
// cutoff modulation locked to the beat. It does NOT alter component heat capacity; the engine's measured
// recovery is authoritative and the requested tau is an approximation (spec).
//
// q quarter-note beats per cycle: divisions 1/32,1/16,1/8,1/4,1/2,1 bar (bar = num*4/den quarters);
// modifiers straight / dotted (x1.5) / triplet (x2/3). D=60q/BPM, tau=D/ln(20) (95% recovery in D).
// BPM clamp 20..300; missing BPM -> 120 + fallback flag. tau smoothed 50 ms.
// Phase=frac(PPQ/q) when PPQ is available, else free-run on the sample clock (continuous across tempo
// changes). PPQ discontinuities (seek/loop) and division changes reset phase from PPQ with the error
// decayed over 20 ms (no jump in the audible modulation).
//
// Threading: process() noexcept, no allocation, audio thread. Block lengths enter only as dt=frames/fs,
// so partitions agree except through block-end sampling of the (smooth) modulation.
#pragma once
#include <optional>

namespace cd::control {

struct TempoInput {
    bool enabled = false;
    int division = 3;       // 0..5 (1/32 .. 1 bar)
    int modifier = 0;       // 0 straight, 1 dotted, 2 triplet
    double depth = 0;       // 0..1 sinusoidal creative cutoff depth
    std::optional<double> bpm;  // nonfinite values are treated as unavailable
    std::optional<double> ppq;  // nonfinite values are treated as unavailable
    int timeSigNum = 4, timeSigDen = 4;
    bool playing = true;
};

struct TempoOutput {
    double recoveryTauS = 0;   // 0 when tempo.enabled is false
    double cutoffOct = 0;      // sinusoidal creative contribution
    bool fallback = false;
    double bpmUsed = 120;
    double phase = 0;          // [0,1) effective phase at block end
    double beatsPerCycle = 1;  // q
};

class TempoBreathing {
public:
    static constexpr double kMaxCutoffOct = 0.15;  // authored creative choice at depth 1
    static constexpr double kTauSmoothS = 0.05;
    static constexpr double kPhaseSmoothS = 0.02;

    struct State {
        double sampleRate = 48000.0;
        double phase = 0, phaseError = 0, recoveryTau = 0;
        bool wasEnabled = false, havePpq = false;
        double lastPpqEnd = 0, lastBeatsPerCycle = 0;
        TempoOutput output{};
    };

    void prepare(double sampleRate) noexcept;
    void reset() noexcept;
    TempoOutput process(const TempoInput& in, int frames) noexcept;  // frames==0: no clock advance
    const TempoOutput& last() const noexcept { return out_; }
    State save() const noexcept;
    bool canRestore(const State& state) const noexcept;
    bool restore(const State& state) noexcept;  // rejection changes nothing

    static double beatsPerCycle(int division, int modifier, int tsNum, int tsDen) noexcept;
    static double recoveryTau(double q, double bpm) noexcept;
    static double clampBpm(double bpm) noexcept;

private:
    double fs_ = 48000.0;
    double phi_ = 0;        // free-run/base phase in cycles
    double phaseErr_ = 0;   // decaying correction (cycles)
    double tauSm_ = 0;
    bool wasEnabled_ = false;
    bool havePpq_ = false;
    double lastPpqEnd_ = 0;
    double lastQ_ = 0;
    TempoOutput out_{};
};

}  // namespace cd::control
