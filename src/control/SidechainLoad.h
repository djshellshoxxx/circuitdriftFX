// cd_control: external sidechain load detector (CD-F16). Produces ONLY a per-sample extra load current
// [A] for the engine's capacitor differential model; it never outputs or mixes sidechain audio.
//
// Sources Main/External/Blend (sidechain.source). Main -> no external current (the engine's own
// I_phys + 3*s*I_dyn applies); External/Blend -> I_ext=depth*I_budget*clamp(E_ext/E_ref,0,4) with
// E_ref = -18 dBFS RMS. Link = max channel magnitude after an optional 2nd-order Butterworth HPF
// (20..300 Hz). Detector: 10 ms mean-square estimate (so a steady sine reads its true RMS) followed by
// attack 1 ms / release 50 ms on the RMS value. Missing/disconnected bus -> zero current + status.
// I_budget comes from the model manifest (amperes); it defaults to 0 (no current) until supplied; no
// guessed units.
//
// Threading: prepare() any thread; process() noexcept, allocation-free, audio thread.
#pragma once

namespace cd::control {

enum class SidechainSource : int { Main = 0, External = 1, Blend = 2 };

class SidechainLoad {
public:
    static constexpr double kRefRms = 0.12589254117941673;  // 10^(-18/20)
    void prepare(double sampleRate) noexcept;
    void reset() noexcept;
    void setBudgetAmps(double amps) noexcept { budget_ = amps > 0 ? amps : 0; }
    double budgetAmps() const noexcept { return budget_; }

    // ext may be null/zero channels => disconnected. Fills `frames` samples of current into out.
    // Returns true when the external bus was used (connected and source != Main).
    bool process(SidechainSource source, double depth, double hpfHz, const double* extL, const double* extR,
                 bool connected, int frames, double* out) noexcept;
    double lastEnergyRatio() const noexcept { return lastRatio_; }  // E_ext/E_ref (unclamped)

private:
    void design(double hz) noexcept;
    struct Biquad {
        double z1 = 0, z2 = 0;
        double run(double x, const double* c) noexcept {
            const double y = c[0] * x + z1;
            z1 = c[1] * x - c[3] * y + z2;
            z2 = c[2] * x - c[4] * y;
            return y;
        }
    };
    double fs_ = 48000.0, hpfHz_ = -1, c_[5] = {1, 0, 0, 0, 0};
    Biquad hp_[2];
    double ms_ = 0, env_ = 0, aMs_ = 0, aAtk_ = 0, aRel_ = 0, budget_ = 0, lastRatio_ = 0;
};

}  // namespace cd::control
