#include "engine/Supply.h"
#include <algorithm>
#include <numbers>

namespace cd::engine {
namespace {
// SYNTHETIC per-unit rectifier laws (G01): forward offset/knee as fractions of the design rail,
// exponent (1.5 Child-law vacuum diode, 1.0 junction), conductance (design currents per p.u.^e),
// regulation (fractional droop at design load; natural recovery tau), lumped reverse-recovery
// charge (design current * 10 ms * value) and heater power [W].
const RectifierLaw kLaws[3] = {
    {0.000, 0.004, 1.5, 354.0, 0.15, 0.0, 10.0},    // Tube5U4G
    {0.020, 0.010, 1.0, 267.0, 0.08, 0.0, 0.0},     // Selenium
    {0.005, 0.001, 1.0, 1000.0, 0.04, 2e-4, 0.0},   // SiliconDiode
};
}  // namespace

const RectifierLaw& rectifierLaw(int type) noexcept { return kLaws[std::clamp(type, 0, 2)]; }

RectifierLaw blendRectifier(const RectifierLaw& a, const RectifierLaw& b, double wb) noexcept {
    auto lin = [&](double x, double y) { return x + wb * (y - x); };
    auto lg = [&](double x, double y) { return std::exp(std::log(x) + wb * (std::log(y) - std::log(x))); };
    return {lin(a.vf, b.vf), lg(a.knee, b.knee), lin(a.exponent, b.exponent), lg(a.conductance, b.conductance),
            lg(a.regulation, b.regulation), lin(a.recoveryChargePu, b.recoveryChargePu), lin(a.heaterW, b.heaterW)};
}

void Supply::prepare(double fs) noexcept {
    dt_ = 1.0 / fs;
    for (double& v : s_) v = 0;
    pRect_ = pRes_ = pRf_ = 0;
}

void Supply::step(double mainsV, double mainsHz, double iStage, double iAux, double tRes, double rLeak,
                  const RectifierLaw& law, double kRec) noexcept {
    constexpr double twoPi = 2.0 * std::numbers::pi;
    double& vRes = s_[kRes];
    double& vFilt = s_[kFilt];
    double& phase = s_[kPhase];
    phase += twoPi * mainsHz * dt_;
    if (phase >= twoPi) phase -= twoPi * std::floor(phase / twoPi);
    const double vSrc = 1.08 * (mainsV / 117.0) * std::fabs(std::sin(phase));
    const double G = law.conductance * kRec, w = law.knee, e = law.exponent;
    const double iLoad = (vRes - vFilt) / kSupplyFilterR + iAux;
    const double cdt = tRes / dt_, gl = 1.0 / rLeak;
    auto rect = [&](double V, double& dIdV) {
        const double z = (vSrc - V - law.vf) / w;
        const double ex = std::exp(-std::fabs(z));
        const double sp = w * (std::max(z, 0.0) + std::log1p(ex));
        const double sg = z >= 0 ? 1.0 / (1.0 + ex) : ex / (1.0 + ex);
        const double pw = e == 1.0 ? 1.0 : std::pow(sp, e - 1.0);
        dIdV = -G * e * pw * sg;
        return G * pw * sp;
    };
    double V = vRes, dI = 0;
    for (int it = 0; it < 8; ++it) {
        const double ir = rect(V, dI);
        const double f = cdt * (V - vRes) - ir + iLoad + V * gl;
        const double dV = f / (cdt - dI + gl);
        V = std::max(0.0, V - dV);
        if (std::fabs(dV) <= 1e-13) break;
    }
    const double ir = rect(V, dI);
    if (!std::isfinite(V) || !std::isfinite(ir)) return;  // keep the last finite state
    const bool conducting = ir > 1e-4;
    if (s_[kCond] > 0.5 && !conducting && law.recoveryChargePu > 0)
        V = std::max(0.0, V - law.recoveryChargePu * 0.01 / tRes);
    s_[kCond] = conducting ? 1.0 : 0.0;
    const double ic = cdt * (V - vRes);
    pRes_ = 1e-4 * ic * ic + V * V * gl;   // ESR 1e-4 p.u.
    pRect_ = ir * std::max(0.0, vSrc - V);
    vRes = V;
    s_[kIr] = ir;
    const double tF = kSupplyFilterTau / kSupplyFilterR;   // filter capacitance time constant C_f * Z_d
    const double cfdt = tF / dt_, gf = 1.0 / kSupplyFilterR;
    const double vf = (cfdt * vFilt + vRes * gf - iStage) / (cfdt + gf);
    if (std::isfinite(vf)) vFilt = std::max(0.0, vf);
    pRf_ = (vRes - vFilt) * (vRes - vFilt) * gf;
}

}  // namespace cd::engine
