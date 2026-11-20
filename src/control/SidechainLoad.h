// cd_control: external sidechain load detector (CD-F16). Produces ONLY a per-sample extra load current
// [A] for the engine's capacitor differential model; it never outputs or mixes sidechain audio.
//
// Sources Main/External/Blend (sidechain.source). Main -> no external current (the engine's own
// I_phys + 3*s*I_dyn applies); External/Blend -> I_ext=depth*I_budget*clamp(E_ext/E_ref,0,4) with
// E_ref = -18 dBFS RMS. Link = max channel magnitude after an optional 2nd-order Butterworth HPF
// (20..300 Hz). Detector: 10 ms mean-square estimate (so a steady sine reads its true RMS) followed by
// attack 1 ms / release 50 ms on the RMS value. A sampled missing/disconnected bus -> zero current
// and a clean detector restart. A zero-frame call preserves all signal history and the last ratio.
// I_budget comes from the model manifest (amperes); it defaults to 0 (no current) until supplied; no
// guessed units.
//
// Numerical domain: nonfinite samples and finite samples outside +/-2^500 contribute zero before
// touching filter history. HPF output/state candidates use the same bound; a step outside it resets
// that channel and contributes zero. This is arithmetic headroom for squared binary64 magnitudes,
// far above audio range, not an audio limiter. The linked mean square, envelope and ratio stay finite.
//
// Threading: prepare() any thread; process() noexcept, allocation-free, audio thread.
#pragma once

namespace cd::control {

enum class SidechainSource : int { Main = 0, External = 1, Blend = 2 };

class SidechainLoad {
public:
    static constexpr double kRefRms = 0.12589254117941673;  // 10^(-18/20)
    // Match the control API: unsupported/nonfinite rates use 48000 Hz (8000..768000 supported).
    // Preparation establishes the default 40 Hz coefficients immediately and clears signal history.
    void prepare(double sampleRate) noexcept;
    void reset() noexcept;
    // Nonfinite/nonpositive -> 0. Finite positive budgets are capped at DBL_MAX/8, solely so
    // maximum depth (2) times maximum load ratio (4) remains representable; no physical cap is guessed.
    void setBudgetAmps(double amps) noexcept;
    double budgetAmps() const noexcept { return budget_; }

    // ext may be null/zero channels => disconnected. Fills `frames` samples of current into out.
    // Returns the current bus-use selection (connected with extL and source != Main).
    // frames <= 0 applies the normalized HPF setting but writes no samples and preserves HPF
    // histories, mean square, envelope and last ratio, regardless of bus/source selection.
    // A positive-frame disconnected/Main block instead writes zero and clears signal history.
    bool process(SidechainSource source, double depth, double hpfHz, const double* extL, const double* extR,
                 bool connected, int frames, double* out) noexcept;
    double lastEnergyRatio() const noexcept { return lastRatio_; }  // E_ext/E_ref (unclamped)

private:
    void design(double hz) noexcept;
    struct Biquad {
        double z1 = 0, z2 = 0;
        double run(double x, const double* c) noexcept;
    };
    double fs_ = 48000.0, hpfHz_ = -1, c_[5] = {1, 0, 0, 0, 0};
    Biquad hp_[2];
    double ms_ = 0, env_ = 0, aMs_ = 0, aAtk_ = 0, aRel_ = 0, budget_ = 0, lastRatio_ = 0;
};

}  // namespace cd::control
