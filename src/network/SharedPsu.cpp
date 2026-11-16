// CircuitDrift network: SYNTHETIC reservoir (see SharedPsu.h).
#include "network/SharedPsu.h"
#include <cmath>
#include "network/Protocol.h"

namespace cd::network {

bool validPsuSpec(const PsuSpec& s) noexcept {
    return std::isfinite(s.nominalRailV) && s.nominalRailV > 0 && std::isfinite(s.sourceV) && s.sourceV > 0 &&
           std::isfinite(s.sourceResistanceOhm) && s.sourceResistanceOhm > 0 && std::isfinite(s.capacitanceF) &&
           s.capacitanceF > 0 && s.manifestId != 0;
}

ReservoirPsu::ReservoirPsu(const PsuSpec& spec, double sampleRate) noexcept : spec_(spec) {
    dt_ = sampleRate > 0 ? double(kQuantumFrames) / sampleRate : 0;
    tau_ = spec.sourceResistanceOhm * spec.capacitanceF;
    if (!(tau_ > 0)) tau_ = 1;
    volts_ = spec.nominalRailV;
}

double ReservoirPsu::trial(double v0, double loadA) const noexcept {
    if (!std::isfinite(loadA) || loadA < 0) loadA = 0;
    if (!std::isfinite(v0)) v0 = spec_.nominalRailV;
    const double C = spec_.capacitanceF, Vs = spec_.sourceV;
    double t = dt_, v = v0;
    if (v > Vs) {                                    // diode off: linear discharge until V_src
        if (loadA <= 0) return v;
        const double tc = (v - Vs) * C / loadA;
        if (tc >= t) return v - loadA * t / C;
        v = Vs;
        t -= tc;
    }
    const double vInf = Vs - loadA * spec_.sourceResistanceOhm;   // conducting steady state
    v = vInf + (v - vInf) * std::exp(-t / tau_);
    return v < 0 ? 0 : v;
}

double ReservoirPsu::stepQuantum(double loadA) noexcept {
    volts_ = trial(volts_, loadA);
    return railFraction();
}

}  // namespace cd::network
