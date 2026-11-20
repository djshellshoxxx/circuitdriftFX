// Shared creative-cutoff domain for the control producer and engine consumer.
// Base knob limits are distinct from effective filter limits. Callers supply a
// prepared sample rate (8..768 kHz); every function is bounded and allocation-free.
#pragma once
#include <algorithm>
#include <cmath>

namespace cd::core {

inline constexpr double kCreativeCutoffOctaves = 2.0;

inline double cutoffBaseHz(double hz) noexcept {
    return std::clamp(std::isfinite(hz) ? hz : 18000.0, 2000.0, 20000.0);
}

struct CutoffDomain {
    double baseHz;
    double minHz;
    double maxHz;
};

inline CutoffDomain cutoffDomain(double baseHz, double sampleRate) noexcept {
    const double ceiling = 0.45 * sampleRate;
    const double effectiveBase = std::min(cutoffBaseHz(baseHz), ceiling);
    // At least two octaves remain even when the knob exceeds the Fs ceiling,
    // so every nonzero 12-bit scale mask admits at least one integer note.
    return {effectiveBase, effectiveBase * 0.25, std::min(effectiveBase * 4.0, ceiling)};
}

inline double applyCreativeCutoff(double baseHz, double octaves, double sampleRate) noexcept {
    const auto domain = cutoffDomain(baseHz, sampleRate);
    const double bounded = std::clamp(std::isfinite(octaves) ? octaves : 0.0,
                                      -kCreativeCutoffOctaves, kCreativeCutoffOctaves);
    return std::min(domain.baseHz * std::exp2(bounded), domain.maxHz);
}

}  // namespace cd::core
