#include "blueprint/Faults.h"
#include <algorithm>
#include <cmath>

namespace cd::blueprint {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kStateBound = 4.0;           // resonator state limit in impulse-peak units
constexpr long kMaxImpulseSamples = 1L << 22;

bool posFinite(double v) noexcept { return std::isfinite(v) && v > 0; }

// Interpolate nominal -> limit in log space by s in [0,1]; exact at both ends, never 0/inf.
double logInterp(double nominal, double limit, double s) noexcept {
    if (s <= 0) return nominal;
    if (s >= 1) return limit;
    return std::exp(std::log(nominal) + (std::log(limit) - std::log(nominal)) * s);
}

double clampSym(double v, double lim) noexcept { return v < -lim ? -lim : v > lim ? lim : v; }

}  // namespace

bool validOverlay(const FaultOverlay& o) noexcept {
    return std::size_t(o.type) < std::size_t(kFaultTypeCount) && std::isfinite(o.severity) && o.severity >= 0 && o.severity <= 1 &&
           std::isfinite(o.frequencyHz) && o.frequencyHz >= 100 && o.frequencyHz <= 4000 && std::isfinite(o.q) && o.q >= 2 && o.q <= 30;
}

bool validBounds(const FaultBounds& b) noexcept {
    return posFinite(b.nominalC) && posFinite(b.minC) && b.minC <= b.nominalC && posFinite(b.nominalEsr) && posFinite(b.openSeriesR) &&
           b.openSeriesR >= b.nominalEsr && posFinite(b.nominalLeakR) && posFinite(b.shortLeakR) && b.shortLeakR <= b.nominalLeakR &&
           posFinite(b.nominalR) && posFinite(b.openR) && b.openR >= b.nominalR && posFinite(b.biasSpanV) &&
           b.openSeriesR < 1e15 && b.openR < 1e15 && b.nominalLeakR < 1e15 && b.minC > 1e-18 && b.shortLeakR > 1e-6;
}

bool faultAvailable(FaultType type, bool isCap, bool isRes, bool isTube, const char*& reason) noexcept {
    reason = "";
    switch (type) {
        case FaultType::ElectrolyticOpen:
        case FaultType::ElectrolyticLeaky:
            if (!isCap) { reason = "requires a capacitor component with declared open/short equivalents"; return false; }
            return true;
        case FaultType::CrackedResistor:
            if (!isRes) { reason = "requires a carbon resistor component with a declared open equivalent"; return false; }
            return true;
        case FaultType::MicrophonicTube:
            if (!isTube) { reason = "no validated tube topology or model is available (G01 open)"; return false; }
            return true;
    }
    reason = "unknown fault type";
    return false;
}

void FaultChannel::retarget(Ramp& r, double targetValue, bool logDomain, long samples) noexcept {
    const double t = logDomain ? std::log(targetValue) : targetValue;
    if (r.target == t) return;   // already there, or already heading there
    r.target = t;
    r.exact = targetValue;       // a completed ramp lands bit-exactly on the declared value
    if (samples <= 0) { r.cur = t; r.left = 0; r.step = 0; r.value = targetValue; return; }
    r.left = samples;
    r.step = (t - r.cur) / double(samples);
}

void FaultChannel::advance(Ramp& r, bool logDomain) noexcept {
    if (r.left <= 0) return;
    if (--r.left == 0) {
        r.cur = r.target;
        r.value = r.exact;
    } else {
        r.cur += r.step;
        r.value = logDomain ? std::exp(r.cur) : r.cur;
    }
}

std::uint64_t FaultChannel::nextRandom() noexcept {  // splitmix64: deterministic from the seed
    rng_ += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = rng_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

void FaultChannel::scheduleNextContact(bool afterLast) noexcept {
    const long base = afterLast ? std::max(sample_, lastToggle_ + minDwell_) : sample_;
    nextToggle_ = base + minDwell_ + long(nextRandom() % std::uint64_t(6 * minDwell_ + 1));
}

void FaultChannel::recomputeTargets(long samples) noexcept {
    const bool live = ov_.enabled && !resetting_;
    const double s = live ? ov_.severity : 0.0;
    double cT = b_.nominalC, eT = b_.nominalEsr, lT = b_.nominalLeakR, rT = b_.nominalR, mT = 0.0;
    if (live) {
        switch (ov_.type) {
            case FaultType::ElectrolyticOpen:
                cT = logInterp(b_.nominalC, b_.minC, s);
                eT = logInterp(b_.nominalEsr, b_.openSeriesR, s);
                break;
            case FaultType::ElectrolyticLeaky: lT = logInterp(b_.nominalLeakR, b_.shortLeakR, s); break;
            case FaultType::CrackedResistor:
                if (!ov_.intermittent || contactOpen_) rT = logInterp(b_.nominalR, b_.openR, s);
                break;
            case FaultType::MicrophonicTube: mT = s; break;
        }
    }
    retarget(c_, cT, true, samples);
    retarget(esr_, eT, true, samples);
    retarget(leak_, lT, true, samples);
    retarget(r_, rT, true, samples);
    retarget(mic_, mT, false, samples);
}

bool FaultChannel::prepare(double electricalRate, const FaultBounds& bounds, const FaultOverlay& overlay, std::string& error) {
    if (!(std::isfinite(electricalRate) && electricalRate >= 8000 && electricalRate <= 6.2e6)) { error = "electrical rate out of range"; return false; }
    if (!validBounds(bounds)) { error = "fault bounds must be finite, positive and ordered nominal->limit"; return false; }
    if (!validOverlay(overlay)) { error = "fault overlay out of range"; return false; }
    if (overlay.frequencyHz >= 0.45 * electricalRate) { error = "resonator frequency too close to Nyquist"; return false; }
    // Resonator: z[n] = 2 r cos(2 pi f/Fs) z[n-1] - r^2 z[n-2] + g x[n], r = exp(-pi f/(Q Fs)).
    const double r = std::exp(-kPi * overlay.frequencyHz / (overlay.q * electricalRate));
    const double a1 = 2.0 * r * std::cos(2.0 * kPi * overlay.frequencyHz / electricalRate), a2 = r * r;
    // Normalise g by the measured peak |impulse response| (g = 1) so a unit impulse peaks at 1.
    const double tau = overlay.q / (kPi * overlay.frequencyHz);
    const long n = std::min(kMaxImpulseSamples, long(std::ceil(16.0 * tau * electricalRate)) + 16);
    double z1 = 0, z2 = 0, peak = 0;
    for (long i = 0; i < n; ++i) {
        const double z = a1 * z1 - a2 * z2 + (i == 0 ? 1.0 : 0.0);
        z2 = z1; z1 = z;
        peak = std::max(peak, std::fabs(z));
    }
    if (!(peak > 0 && std::isfinite(peak))) { error = "resonator impulse response invalid"; return false; }

    b_ = bounds;
    ov_ = overlay;
    rate_ = electricalRate;
    preparedF_ = overlay.frequencyHz;
    preparedQ_ = overlay.q;
    a1_ = a1; a2_ = a2; g_ = 1.0 / peak; z1_ = z2_ = 0;
    valueRamp_ = std::max(1L, long(std::llround(kValueRampSeconds * rate_)));
    contactRamp_ = std::max(1L, long(std::llround(kContactRampSeconds * rate_)));
    resetRamp_ = std::max(1L, long(std::llround(kResetRampSeconds * rate_)));
    minDwell_ = std::max(1L, long(std::ceil(kMinContactDwellSeconds * rate_)));
    resetting_ = false; resetLeft_ = 0;
    contactOpen_ = false;
    sample_ = 0; transitions_ = 0; lastToggle_ = -1000000000L;
    rng_ = overlay.seed;
    c_ = esr_ = leak_ = r_ = mic_ = Ramp{};
    c_.cur = c_.target = std::log(b_.nominalC); c_.value = c_.exact = b_.nominalC;
    esr_.cur = esr_.target = std::log(b_.nominalEsr); esr_.value = esr_.exact = b_.nominalEsr;
    leak_.cur = leak_.target = std::log(b_.nominalLeakR); leak_.value = leak_.exact = b_.nominalLeakR;
    r_.cur = r_.target = std::log(b_.nominalR); r_.value = r_.exact = b_.nominalR;
    recomputeTargets(0);
    if (ov_.enabled && ov_.type == FaultType::CrackedResistor && ov_.intermittent && ov_.severity > 0) scheduleNextContact(false);
    clearedDone_ = !(ov_.enabled && ov_.severity > 0);
    prepared_ = true;
    error.clear();
    return true;
}

bool FaultChannel::apply(const FaultOverlay& o) noexcept {
    if (!prepared_ || !validOverlay(o) || o.revision < ov_.revision) return false;
    if (o.frequencyHz != preparedF_ || o.q != preparedQ_) return false;  // g must be re-measured in prepare()
    const bool reseed = o.seed != ov_.seed;
    const bool wasIntermittent = ov_.enabled && ov_.type == FaultType::CrackedResistor && ov_.intermittent && ov_.severity > 0 && !resetting_;
    ov_ = o;
    resetting_ = false; resetLeft_ = 0;
    if (reseed) rng_ = ov_.seed;
    const bool nowIntermittent = ov_.enabled && ov_.type == FaultType::CrackedResistor && ov_.intermittent && ov_.severity > 0;
    if (!nowIntermittent) contactOpen_ = false;
    else if (!wasIntermittent || reseed) scheduleNextContact(true);
    if (ov_.type != FaultType::MicrophonicTube) { z1_ = z2_ = 0; }
    clearedDone_ = !(ov_.enabled && ov_.severity > 0);
    recomputeTargets(valueRamp_);
    return true;
}

void FaultChannel::reset() noexcept {
    if (!prepared_ || (clearedDone_ && !resetting_)) return;
    resetting_ = true;
    resetLeft_ = resetRamp_;
    contactOpen_ = false;
    recomputeTargets(resetRamp_);
}

FaultOutputs FaultChannel::tick(double excitation) noexcept {
    FaultOutputs out;
    if (!prepared_) return out;
    ++sample_;
    if (resetting_ && --resetLeft_ <= 0) {   // ramps (same length) complete on this sample
        resetting_ = false;
        ov_.enabled = false;
        ov_.severity = 0;
        z1_ = z2_ = 0;
        contactOpen_ = false;
        clearedDone_ = true;
    }
    if (!resetting_ && ov_.enabled && ov_.type == FaultType::CrackedResistor && ov_.intermittent && ov_.severity > 0 &&
        sample_ >= nextToggle_) {
        contactOpen_ = !contactOpen_;
        ++transitions_;
        lastToggle_ = sample_;
        scheduleNextContact(false);
        recomputeTargets(contactRamp_);
    }
    advance(c_, true); advance(esr_, true); advance(leak_, true); advance(r_, true); advance(mic_, false);
    out.capacitanceF = c_.value;
    out.seriesROhm = esr_.value;
    out.leakageROhm = leak_.value;
    out.resistanceOhm = r_.value;
    out.contactOpen = contactOpen_;
    // Keep every value strictly inside the declared equivalents (never ideal 0/inf).
    out.capacitanceF = std::clamp(out.capacitanceF, b_.minC, b_.nominalC);
    out.seriesROhm = std::clamp(out.seriesROhm, b_.nominalEsr, b_.openSeriesR);
    out.leakageROhm = std::clamp(out.leakageROhm, b_.shortLeakR, b_.nominalLeakR);
    out.resistanceOhm = std::clamp(out.resistanceOhm, b_.nominalR, b_.openR);

    if (ov_.type == FaultType::MicrophonicTube && (ov_.enabled || mic_.left > 0 || mic_.value != 0)) {
        double x = std::isfinite(excitation) ? excitation : 0.0;
        x = x < 0 ? 0 : x > 1 ? 1 : x;
        double z = a1_ * z1_ - a2_ * z2_ + g_ * x;
        z = clampSym(z, kStateBound);
        z2_ = z1_; z1_ = z;
        const double scale = std::clamp(mic_.value, 0.0, 1.0);
        out.biasPerturbationV = kBiasPerturbationMax * scale * b_.biasSpanV * clampSym(z, 1.0);
    }
    return out;
}

}  // namespace cd::blueprint
