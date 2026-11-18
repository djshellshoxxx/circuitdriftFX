#include "dsp/CircuitStage.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace cd::dsp {
namespace {
double sanitize(double x, long& repairs) noexcept {
    if (!std::isfinite(x)) { ++repairs; return 0.0; }
    if (x > CircuitStage::kInputClamp) { ++repairs; return CircuitStage::kInputClamp; }
    if (x < -CircuitStage::kInputClamp) { ++repairs; return -CircuitStage::kInputClamp; }
    return x;
}
}  // namespace

bool CircuitStage::prepare(const Config& cfg, std::string& error) {
    auto bad = [&](const char* why) { error = why; return false; };
    if (!(std::isfinite(cfg.fs) && cfg.fs > 0)) return bad("sample rate must be finite and positive");
    if (!(std::isfinite(cfg.inputVoltsPerFs) && cfg.inputVoltsPerFs > 0)) return bad("inputVoltsPerFs must be finite and positive");
    if (!(std::isfinite(cfg.outputVoltsPerFs) && cfg.outputVoltsPerFs > 0 && std::isfinite(1.0 / cfg.outputVoltsPerFs)))
        return bad("outputVoltsPerFs must be finite and positive with a finite reciprocal");
    if (!(std::isfinite(cfg.fadeSeconds) && cfg.fadeSeconds > 0 && cfg.fadeSeconds <= 1.0)) return bad("fadeSeconds must be in (0,1]");
    if (!(std::isfinite(cfg.thermalHz) && cfg.thermalHz > 0 && cfg.thermalHz <= cfg.fs)) return bad("thermalHz must be in (0,fs]");
    if (!(std::isfinite(cfg.ambientK) && cfg.ambientK > 0)) return bad("ambient must be finite and positive");
    if (cfg.latencyTarget < 0 || cfg.latencyTarget > kMaxLatency) return bad("latencyTarget out of range");
    if (cfg.inputSource < 0 || cfg.inputSource >= int(cfg.net.elements.size()) ||
        cfg.net.elements[size_t(cfg.inputSource)].kind != NetElement::Kind::VoltageSource)
        return bad("inputSource must index a VoltageSource element");
    if (cfg.outputNode < 1 || cfg.outputNode > cfg.net.nodes) return bad("outputNode outside 1..nodes");
    if (!cfg.elementThermalNode.empty()) {
        if (cfg.elementThermalNode.size() != cfg.net.elements.size()) return bad("elementThermalNode size must match elements");
        for (int n : cfg.elementThermalNode)
            if (n < -1 || (n >= 0 && (cfg.thermal.n == 0 || std::size_t(n) >= cfg.thermal.n))) return bad("elementThermalNode outside thermal nodes");
    }

    CircuitStage t;  // build aside; commit only on full success
    if (!t.os_.prepare(cfg.os, 1)) return bad("oversampler specification rejected");
    t.factor_ = t.os_.factor();
    const double lat = t.os_.latency();
    if (!std::isfinite(lat) || lat < 0 || lat > kMaxLatency) return bad("oversampler latency out of range");
    const long latHi = std::lround(lat * t.factor_);
    const int minLatency = int(std::ceil(lat - 1e-9));
    t.latency_ = cfg.latencyTarget > 0 ? cfg.latencyTarget : minLatency;
    if (t.latency_ < minLatency) return bad("latencyTarget below the oversampler latency");
    const long padLong = long(t.latency_) * t.factor_ - latHi;
    if (padLong < 0) return bad("latency padding negative");
    t.padLen_ = int(padLong);

    t.h_ = 1.0 / (cfg.fs * double(t.factor_));
    if (!t.sol_.prepare(cfg.net, t.h_, cfg.newton, error)) return false;
    t.initState_.resize(size_t(t.sol_.stateSize()));
    t.sol_.getState(t.initState_.data());

    t.thermalN_ = int(cfg.thermal.n);
    if (t.thermalN_ > 0) {
        if (!t.therm_.prepare(cfg.thermal, 1.0 / cfg.thermalHz, cfg.ambientK)) return bad("thermal network rejected");
        t.initTempK_.resize(cfg.thermal.n);
        for (std::size_t i = 0; i < cfg.thermal.n; ++i) t.initTempK_[i] = t.therm_.temperature(i);
    }
    t.power_.assign(cfg.net.elements.size(), PowerElem{});
    for (std::size_t k = 0; k < cfg.net.elements.size(); ++k) {
        const NetElement& e = cfg.net.elements[k];
        PowerElem& p = t.power_[k];
        if (e.kind != NetElement::Kind::Device || !e.model) continue;
        p.a = e.a; p.b = e.b; p.p0 = e.param[0]; p.p1 = e.param[1]; p.p2 = e.param[2]; p.cap = e.model->maxExpArg;
        p.node = cfg.elementThermalNode.empty() ? 0 : cfg.elementThermalNode[k];
        if (p.node < 0) continue;
        if (e.model->equation == DeviceEquation::Resistor) p.k = PowerElem::K::Resistor;
        else if (e.model->equation == DeviceEquation::ShockleyDiode) p.k = PowerElem::K::Diode;
    }

    t.dry_.assign(size_t(t.latency_), 0.0);
    t.pad_.assign(size_t(t.padLen_), 0.0);
    t.hi_.assign(size_t(t.factor_), 0.0);
    t.fadeN_ = std::max(1, int(std::lround(cfg.fadeSeconds * cfg.fs)));
    t.period_ = cfg.fs / cfg.thermalHz;
    t.src_ = cfg.inputSource; t.outNode_ = cfg.outputNode;
    t.inV_ = cfg.inputVoltsPerFs; t.invOutV_ = 1.0 / cfg.outputVoltsPerFs;
    t.prepared_ = true;
    t.reset();
    *this = std::move(t);
    error.clear();
    return true;
}

void CircuitStage::reset() noexcept {
    if (prepared_) {
        os_.reset();
        sol_.setState(initState_.data());
        if (thermalN_ > 0) therm_.setState(initTempK_.data());
    }
    std::fill(dry_.begin(), dry_.end(), 0.0);
    std::fill(pad_.begin(), pad_.end(), 0.0);
    std::fill(hi_.begin(), hi_.end(), 0.0);
    std::fill(std::begin(energy_), std::end(energy_), 0.0);
    accTime_ = phase_ = 0;
    dryPos_ = padPos_ = 0;
    state_ = StageState::Running; reason_ = FallbackReason::None; fallbackAt_ = -1;
    fadePos_ = failStreak_ = 0; haveValid_ = false;
    lastWet_ = lastHi_ = 0;
    stepFailures_ = inputRepairs_ = 0; hostSamples_ = thermalTicks_ = 0;
}

double CircuitStage::dissipation(const PowerElem& e) const noexcept {
    const double v = sol_.voltage(e.a) - sol_.voltage(e.b);
    if (e.k == PowerElem::K::Resistor) return v * v / e.p0;
    // Same C1 linear-extended Shockley equation as MnaSolver::evaluate (kept in sync by the DC test).
    const double nvt = e.p1 * e.p2, arg = v / nvt;
    double i;
    if (arg > e.cap) { const double ec = std::exp(e.cap); i = e.p0 * (ec * (1.0 + arg - e.cap) - 1.0); }
    else i = e.p0 * (std::exp(arg) - 1.0);
    return v * i;
}

void CircuitStage::startFade(FallbackReason why) noexcept {
    state_ = StageState::Fading;
    reason_ = why;
    fadePos_ = 0;
    fallbackAt_ = std::int64_t(hostSamples_) - 1;  // hostSamples_ already counts this sample
}

double CircuitStage::sample(double xIn) noexcept {
    const double x = sanitize(xIn, inputRepairs_);
    ++hostSamples_;
    // Dry path: integer delay of the declared latency.
    double dry = x;
    if (!dry_.empty()) {
        dry = dry_[dryPos_];
        dry_[dryPos_] = x;
        if (++dryPos_ == dry_.size()) dryPos_ = 0;
    }
    double out = dry;
    if (prepared_ && state_ != StageState::DryFallback) {
        os_.up(&x, hi_.data(), 1);
        bool noValid = false;
        for (int j = 0; j < factor_; ++j) {
            sol_.setSource(src_, hi_[size_t(j)] * inV_);
            bool ok = sol_.step();
            double o = sol_.voltage(outNode_) * invOutV_;
            if (!std::isfinite(o)) { o = lastHi_; ok = false; }
            if (ok) { failStreak_ = 0; haveValid_ = true; }
            else {
                ++failStreak_;
                if (stepFailures_ < 0x7fffffffffffffffL) ++stepFailures_;
                if (!haveValid_) { noValid = true; break; }
            }
            o = std::min(kHiRateClamp, std::max(-kHiRateClamp, o));
            lastHi_ = o;
            if (thermalN_ > 0) {
                for (const PowerElem& e : power_) {
                    if (e.k == PowerElem::K::None) continue;
                    const double p = dissipation(e);
                    if (std::isfinite(p)) energy_[e.node] += p * h_;
                }
                accTime_ += h_;
            }
            hi_[size_t(j)] = o;
            if (failStreak_ >= kFailuresToFallback && state_ == StageState::Running) startFade(FallbackReason::SolverFailures);
        }
        if (noValid) {
            // No prior valid sample: start from delayed dry immediately (spec), no fade.
            state_ = StageState::DryFallback; reason_ = FallbackReason::NoValidSample;
            fallbackAt_ = std::int64_t(hostSamples_) - 1;
        } else {
            if (padLen_ > 0) {  // fractional-delay compensation in the oversampled domain
                for (int j = 0; j < factor_; ++j) {
                    const double older = pad_[padPos_];
                    pad_[padPos_] = hi_[size_t(j)];
                    hi_[size_t(j)] = older;
                    if (++padPos_ == pad_.size()) padPos_ = 0;
                }
            }
            double wet = 0;
            os_.down(hi_.data(), &wet, 1);
            FallbackReason why = FallbackReason::None;
            if (!std::isfinite(wet)) { wet = lastWet_; why = FallbackReason::NonFiniteWet; }
            else if (std::fabs(wet) > kEmergencyAmplitude) { wet = std::copysign(kEmergencyAmplitude, wet); why = FallbackReason::Overamplitude; }
            if (why != FallbackReason::None && state_ == StageState::Running) startFade(why);
            lastWet_ = wet;
            if (state_ == StageState::Running) out = wet;
            else {
                ++fadePos_;
                if (fadePos_ >= fadeN_) { state_ = StageState::DryFallback; out = dry; }
                else {
                    const double w = double(fadeN_ - fadePos_) / double(fadeN_);  // linear equal-sum weights
                    out = w * wet + (1.0 - w) * dry;
                }
            }
        }
    }
    // Thermal clock: fractional phase accumulator in host samples, independent of block boundaries.
    phase_ += 1.0;
    if (phase_ >= period_) {
        phase_ -= period_;
        if (thermalN_ > 0) {
            double p[ThermalNetwork::kMaxNodes];
            for (int i = 0; i < thermalN_; ++i) p[i] = accTime_ > 0 ? energy_[i] / accTime_ : 0.0;
            therm_.step(p);
            std::fill(std::begin(energy_), std::end(energy_), 0.0);
            accTime_ = 0;
        }
        ++thermalTicks_;
    }
    return out;
}

void CircuitStage::process(const double* in, double* out, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) out[i] = sample(in[i]);
}

}  // namespace cd::dsp
