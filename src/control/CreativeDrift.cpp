#include "control/CreativeDrift.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace cd::control {

void CreativeDrift::prepare(double sampleRate, uint64_t instanceSeed, uint32_t slot) noexcept {
    const double fs = std::isfinite(sampleRate) && sampleRate >= 8000.0 && sampleRate <= 768000.0
                          ? sampleRate : 48000.0;
    // A supported binary64 rate is an integer significand divided by 2^(53-exponent).
    // Reduce 100/fs to integer terms so neither fractional rates nor large sample counts
    // lose tick boundaries. No extended-precision floating point or 128-bit integers needed.
    int exponent = 0;
    const double fraction = std::frexp(fs, &exponent);
    const uint64_t period = uint64_t(std::ldexp(fraction, 53));
    const uint64_t step = uint64_t(kClockHz) << (53 - exponent);
    const uint64_t divisor = std::gcd(period, step);
    clockPeriod_ = period / divisor;
    clockStep_ = step / divisor;
    samplePhase_ = 0;
    st_ = State{};
    for (uint32_t j = 0; j < 3; ++j) {
        rng_[j].seed(deriveSeed(instanceSeed, slot, j));
        st_.u[j] = rng_[j].gaussian();  // stationary start: u ~ N(0,1)
    }
    st_.q = std::tanh((st_.u[0] + st_.u[1] + st_.u[2]) / std::sqrt(3.0));
}

bool CreativeDrift::validateState(const State& s, uint64_t& phase) const noexcept {
    if (!std::isfinite(s.q) || std::fabs(s.q) > 1.0 || !std::isfinite(s.delayUnitMs) ||
        std::fabs(s.delayUnitMs) > kMaxDelayMs) return false;
    double sum = 0;
    for (int j = 0; j < 3; ++j) {
        const auto& rng = s.rng[j];
        // Box-Muller draws are bounded below 9 by the 53-bit uniform input. For all
        // three AR coefficients that bounds reachable process magnitude below 1024.
        if (!std::isfinite(s.u[j]) || std::fabs(s.u[j]) > 1024.0 ||
            !std::isfinite(rng.spare) || std::fabs(rng.spare) > 9.0 || rng.hasSpare > 1 ||
            (rng.s[0] | rng.s[1] | rng.s[2] | rng.s[3]) == 0) return false;
        sum += s.u[j];
    }
    if (std::fabs(s.q - std::tanh(sum / std::sqrt(3.0))) > 16 * std::numeric_limits<double>::epsilon()) return false;
    return s.ticks == clockPosition(s.samples, phase);
}

bool CreativeDrift::canRestore(const State& s) const noexcept {
    uint64_t phase = 0;
    return validateState(s, phase);
}

void CreativeDrift::restore(const State& s) noexcept {
    uint64_t phase = 0;
    if (!validateState(s, phase)) return;
    st_ = s;
    samplePhase_ = phase;
    for (int j = 0; j < 3; ++j) rng_[j].setState(st_.rng[j]);
}

uint64_t CreativeDrift::clockPosition(uint64_t samples, uint64_t& phase) const noexcept {
    // Divide samples*clockStep_ by clockPeriod_ one input bit at a time. The phase
    // intermediate is below 3*clockPeriod_ (<2^55); tick count is no larger than samples.
    // This fixed 64-step validation cannot execute a checkpoint's historical tick backlog.
    uint64_t ticks = 0;
    phase = 0;
    for (int bit = 63; bit >= 0; --bit) {
        phase = 2 * phase + ((samples >> bit) & 1) * clockStep_;
        ticks = 2 * ticks + phase / clockPeriod_;
        phase %= clockPeriod_;
    }
    return ticks;
}

void CreativeDrift::tick() noexcept {
    double sum = 0;
    for (int j = 0; j < 3; ++j) {
        const double a = std::exp(-(1.0 / kClockHz) / kTau[j]);
        st_.u[j] = a * st_.u[j] + std::sqrt(1.0 - a * a) * rng_[j].gaussian();
        sum += st_.u[j];
    }
    st_.q = std::tanh(sum / std::sqrt(3.0));
    const double target = kMaxDelayMs * st_.q;
    const double maxStep = kSlewMsPerS / kClockHz;
    st_.delayUnitMs += std::clamp(target - st_.delayUnitMs, -maxStep, maxStep);
}

void CreativeDrift::advance(int frames) noexcept {
    if (frames <= 0) return;
    uint64_t left = std::min(uint64_t(frames), std::numeric_limits<uint64_t>::max() - st_.samples);
    st_.samples += left;
    while (left > 0) {
        const uint64_t need = clockPeriod_ - samplePhase_;
        const uint64_t untilTick = need / clockStep_ + (need % clockStep_ != 0);
        const uint64_t step = std::min(left, untilTick);
        // Skip to the next boundary. The product is bounded by period+step (<2^54),
        // and every iteration consumes at least one supplied frame.
        samplePhase_ += step * clockStep_;
        left -= step;
        if (samplePhase_ >= clockPeriod_) {
            samplePhase_ -= clockPeriod_;
            tick();
            ++st_.ticks;
        }
    }
}

}  // namespace cd::control
