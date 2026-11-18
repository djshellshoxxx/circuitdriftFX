// CircuitDrift engine: reduced supply model per slot (01-runtime "calibrated reduced supply model").
// SYNTHETIC FIXTURE (G01): per-unit rectifier I-V laws, not measured rectifier data.
//
// Per-unit form (shared by every active-element family of a slot, so an element crossfade never
// jumps the reservoir): v = V / V_d, i = I / I_d, with the family's design rail V_d and design
// current I_d (Z_d = V_d / I_d). Capacitances enter as time constants C * Z_d [s].
//   source    v_src = 1.08 * (mains / 117 V) * |sin(phi)|  (full-wave; phi shared with mains hum)
//   rectifier i_r = G * k_rec * (w * softplus((v_src - v_res - v_f) / w))^e
//   reservoir T_res dv_res/dt = i_r - i_load - v_res / r_leak            (backward Euler, Newton)
//   filter    T_f dv_f/dt = (v_res - v_f) / r_f - i_stage                (backward Euler, exact)
//   i_load = (v_res - v_f) / r_f + i_aux,  i_aux = idle + sag*K*i_dyn + creative (3s i_dyn | i_ext)
// k_rec retimes recovery (tempo sync) = clamp(tau_nat / tau_target, 0.05, 20), tau_nat =
// T_res * regulation (an approximation; displayed measured recovery is authoritative). Junction
// rectifiers remove a lumped reverse-recovery charge when conduction ends. Runs once per host
// sample: stages see v_f from the previous host sample (one-host-sample lag, documented).
#pragma once
#include <cmath>

namespace cd::engine {

struct RectifierLaw { double vf, knee, exponent, conductance, regulation, recoveryChargePu, heaterW; };
const RectifierLaw& rectifierLaw(int type) noexcept;   // 0 Tube5U4G, 1 Selenium, 2 SiliconDiode
RectifierLaw blendRectifier(const RectifierLaw& a, const RectifierLaw& b, double wb) noexcept;

constexpr double kSupplyFilterR = 0.1;     // r_f [p.u.]
constexpr double kSupplyFilterTau = 0.05;  // r_f * T_f [s]

class Supply {
public:
    void prepare(double fs) noexcept;
    void step(double mainsV, double mainsHz, double iStage, double iAux, double tRes, double rLeak,
              const RectifierLaw& law, double kRec) noexcept;
    double reservoir() const noexcept { return s_[kRes]; }
    double filter() const noexcept { return s_[kFilt]; }
    double rectifierCurrent() const noexcept { return s_[kIr]; }
    double phase() const noexcept { return s_[kPhase]; }
    // Dissipation this sample in p.u. (multiply by V_d * I_d for watts).
    double rectifierLoss() const noexcept { return pRect_; }
    double reservoirLoss() const noexcept { return pRes_; }
    double filterResistorLoss() const noexcept { return pRf_; }
    static double naturalTau(double tRes, const RectifierLaw& law) noexcept { return tRes * law.regulation; }
    void setState(double vRes, double vFilt, double phase) noexcept { s_[kRes] = vRes; s_[kFilt] = vFilt; s_[kPhase] = phase; s_[kIr] = 0; s_[kCond] = 0; }
    static constexpr int kStateSize = 5;   // reservoir, filter, mains phase, rectifier current, conduction
    double* state() noexcept { return s_; }
private:
    enum { kRes, kFilt, kPhase, kIr, kCond };
    double dt_ = 1.0 / 48000;
    double s_[kStateSize] = {};
    double pRect_ = 0, pRes_ = 0, pRf_ = 0;
};

}  // namespace cd::engine
