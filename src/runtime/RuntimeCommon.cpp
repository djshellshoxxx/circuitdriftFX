// CircuitDrift runtime: shared helper implementations (see RuntimeCommon.h). Owner: cd_runtime.
#include "runtime/RuntimeCommon.h"
#include <algorithm>
#include <cmath>

namespace cd::runtime {

void DelayLine::prepare(int delaySamples) {
    d_ = delaySamples < 0 ? 0 : delaySamples;
    buf_.assign(std::size_t(d_ > 0 ? d_ : 1), 0.0);
    pos_ = 0;
}

void DelayLine::reset() noexcept {
    std::fill(buf_.begin(), buf_.end(), 0.0);
    pos_ = 0;
}

void OnePole::prepare(double fs, double tauSec) noexcept {
    a_ = (fs > 0 && tauSec > 0 && std::isfinite(fs) && std::isfinite(tauSec)) ? std::exp(-1.0 / (fs * tauSec)) : 0.0;
}

bool planLatency(const dsp::Oversampler::Spec& spec, bool padToFamilyMax, LatencyPlan& out) {
    auto latencyOf = [&](int factor, double& lat) {
        dsp::Oversampler os;
        dsp::Oversampler::Spec s = spec;
        s.factor = factor;
        if (!os.prepare(s, 1)) return false;
        lat = os.latency();
        return std::isfinite(lat) && lat >= 0;
    };
    LatencyPlan p;
    if (!latencyOf(spec.factor, p.osLatency)) return false;
    double target = p.osLatency;
    if (padToFamilyMax) {
        for (int f : {1, 2, 4, 8, 16}) {
            double l = 0;
            if (!latencyOf(f, l)) return false;
            target = std::max(target, l);
        }
        p.familyMax = target;
    }
    p.declared = int(std::ceil(target - 1e-9));
    const double padBase = double(p.declared) - p.osLatency;
    const double padHigh = std::round(padBase * double(spec.factor));
    if (!(padHigh >= 0) || padHigh > 1e6) return false;
    p.padHigh = int(padHigh);
    p.residual = padBase - padHigh / double(spec.factor);
    if (spec.family == dsp::OsFamily::Fir && std::fabs(p.residual) > 1e-9) return false;  // FIR must be exact
    out = p;
    return true;
}

}  // namespace cd::runtime
