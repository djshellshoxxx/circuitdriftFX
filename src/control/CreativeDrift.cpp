#include "control/CreativeDrift.h"
#include <algorithm>
#include <cmath>

namespace cd::control {

void CreativeDrift::prepare(double sampleRate, uint64_t instanceSeed, uint32_t slot) noexcept {
    fs_ = sampleRate > 0 ? sampleRate : 48000.0;
    st_ = State{};
    for (uint32_t j = 0; j < 3; ++j) {
        rng_[j].seed(deriveSeed(instanceSeed, slot, j));
        st_.u[j] = rng_[j].gaussian();  // stationary start: u ~ N(0,1)
    }
    st_.q = std::tanh((st_.u[0] + st_.u[1] + st_.u[2]) / std::sqrt(3.0));
}

void CreativeDrift::restore(const State& s) noexcept {
    st_ = s;
    for (int j = 0; j < 3; ++j) rng_[j].setState(st_.rng[j]);
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
    st_.samples += uint64_t(frames);
    const auto target = uint64_t(std::floor(double(st_.samples) * kClockHz / fs_));
    while (st_.ticks < target) {
        tick();
        ++st_.ticks;
    }
}

}  // namespace cd::control
