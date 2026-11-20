// Synthetic API regressions for sidechain history and numerical admission. No physical or
// audible-plugin claim: the adapter does not currently supply a nonzero sidechain budget.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>
#include "TestHarness.h"
#include "control/ControlProcessor.h"
#include "control/SidechainLoad.h"

namespace sidechain_regression {
using namespace cd;
using namespace cd::control;
constexpr double kRate = 48000.0;
constexpr double kBudget = 0.02;
constexpr int kFrames = 1024;

inline bool finiteCurrent(const std::vector<double>& values, double maximum) {
    return std::all_of(values.begin(), values.end(), [maximum](double value) {
        return std::isfinite(value) && value >= 0.0 && value <= maximum;
    });
}

inline bool exactZero(const std::vector<double>& values) {
    return std::all_of(values.begin(), values.end(), [](double value) {
        return std::isfinite(value) && value == 0.0;
    });
}

inline void detectorZeroFrames() {
    const auto left = th::sine(733.0, 0.19, kRate, kFrames);
    const auto right = th::sine(1373.0, 0.11, kRate, kFrames, 0.4);
    const auto suffix = th::sine(421.0, 0.07, kRate, kFrames, 0.2);
    for (const auto source : {SidechainSource::External, SidechainSource::Blend}) {
        SidechainLoad uninterrupted, flushed, fresh;
        for (auto* detector : {&uninterrupted, &flushed, &fresh}) {
            detector->prepare(kRate);
            detector->setBudgetAmps(kBudget);
        }
        std::vector<double> expected(kFrames), actual(kFrames);
        uninterrupted.process(source, 0.8, 88.0, left.data(), right.data(), true, kFrames, expected.data());
        flushed.process(source, 0.8, 88.0, left.data(), right.data(), true, kFrames, actual.data());
        const double before = flushed.lastEnergyRatio();
        CHECK(expected == actual && before > 0.0, "sidechain zero-frame fixture is warm, source %d", int(source));

        std::array<double, 2> untouched{-71.0, 29.0};
        CHECK(!flushed.process(source, 0.8, 240.0, nullptr, nullptr, false, 0, untouched.data()),
              "zero-frame bus-use result reflects the supplied missing bus");
        CHECK(flushed.lastEnergyRatio() == before && untouched[0] == -71.0 && untouched[1] == 29.0,
              "no-bus zero-frame call preserves energy and writes no samples, source %d", int(source));
        CHECK(!flushed.process(SidechainSource::Main, 0.0, 37.0, left.data(), right.data(), true, 0, nullptr),
              "zero-frame Main selection reports no external use");
        CHECK(flushed.lastEnergyRatio() == before, "zero-frame Main selection preserves detector history");
        CHECK(flushed.process(source, 0.8, 88.0, left.data(), right.data(), true, 0, nullptr),
              "connected zero-frame call reports the current external selection");
        uninterrupted.process(source, 0.8, 88.0, suffix.data(), right.data(), true, kFrames, expected.data());
        flushed.process(source, 0.8, 88.0, suffix.data(), right.data(), true, kFrames, actual.data());
        CHECK(actual == expected && flushed.lastEnergyRatio() == uninterrupted.lastEnergyRatio(),
              "all suffix currents and final ratio are identical after zero-frame calls, source %d", int(source));

        std::fill(actual.begin(), actual.end(), -1.0);
        CHECK(!flushed.process(source, 0.8, 88.0, nullptr, nullptr, false, 17, actual.data()),
              "positive-frame disconnect reports no external use");
        CHECK(std::all_of(actual.begin(), actual.begin() + 17, [](double value) { return value == 0.0; }) &&
                  actual[17] == -1.0 && flushed.lastEnergyRatio() == 0.0,
              "positive-frame disconnect clears history and writes only its zero-current span");
        fresh.process(source, 0.8, 88.0, suffix.data(), right.data(), true, kFrames, expected.data());
        flushed.process(source, 0.8, 88.0, suffix.data(), right.data(), true, kFrames, actual.data());
        CHECK(actual == expected && flushed.lastEnergyRatio() == fresh.lastEnergyRatio(),
              "reconnection after positive-frame disconnect starts exactly clean");

        CHECK(!flushed.process(SidechainSource::Main, 0.8, 88.0, left.data(), right.data(), true, kFrames, actual.data()) &&
                  exactZero(actual) && flushed.lastEnergyRatio() == 0.0,
              "positive-frame Main selection clears history and current");
        fresh.reset();
        fresh.process(source, 0.8, 88.0, suffix.data(), right.data(), true, kFrames, expected.data());
        flushed.process(source, 0.8, 88.0, suffix.data(), right.data(), true, kFrames, actual.data());
        CHECK(actual == expected, "external selection after positive-frame Main starts exactly clean");
    }
}

inline void processorZeroFrames() {
    const auto left = th::sine(733.0, 0.19, kRate, kFrames);
    const auto right = th::sine(1373.0, 0.11, kRate, kFrames, 0.4);
    const double* channels[2] = {left.data(), right.data()};
    const AudioView external{channels, 2};
    for (const int source : {1, 2}) {
        ControlProcessor uninterrupted, flushed, fresh;
        for (auto* processor : {&uninterrupted, &flushed, &fresh}) {
            processor->prepare(kRate, kFrames, 67);
            processor->setSidechainBudgetA(kBudget);
        }
        auto parameters = core::defaultValues();
        parameters[core::indexOf("sidechain.source")] = source;
        parameters[core::indexOf("sidechain.depth")] = 0.8;
        parameters[core::indexOf("sidechain.hpf")] = 88.0;
        ControlContext context;
        context.frames = kFrames;
        core::ModulationBlock expected, actual, zero;
        for (int i = 0; i < 4; ++i) {
            uninterrupted.processBlock(parameters, context, AudioView{}, &external, expected);
            flushed.processBlock(parameters, context, AudioView{}, &external, actual);
        }
        CHECK(actual.externalLoadA[kFrames - 1] > 0.0, "control zero-frame fixture is warm, source %d", source);
        context.frames = 0;
        flushed.processBlock(parameters, context, AudioView{}, nullptr, zero);
        // Status describes the current callback's supplied view even when it has no samples;
        // this parameter/status update must not be interpreted as a sampled disconnection.
        CHECK(flushed.status().sidechainMissing && !flushed.status().sidechainConnected &&
                  zero.slot[0].sidechainMode == source,
              "zero-frame control status and mode reflect the current no-bus call, source %d", source);
        auto mainParameters = parameters;
        mainParameters[core::indexOf("sidechain.source")] = 0.0;
        mainParameters[core::indexOf("sidechain.hpf")] = 240.0;
        flushed.processBlock(mainParameters, context, AudioView{}, nullptr, zero);
        CHECK(!flushed.status().sidechainMissing && zero.slot[0].sidechainMode == 0,
              "zero-frame Main parameter update is visible without consuming samples");
        context.frames = kFrames;
        uninterrupted.processBlock(parameters, context, AudioView{}, &external, expected);
        flushed.processBlock(parameters, context, AudioView{}, &external, actual);
        CHECK(std::equal(expected.externalLoadA, expected.externalLoadA + kFrames, actual.externalLoadA),
              "control suffix currents exactly survive no-bus zero-frame calls, source %d", source);
        CHECK(flushed.status().sidechainConnected && !flushed.status().sidechainMissing,
              "resumed control status reflects the supplied connected bus");

        flushed.processBlock(parameters, context, AudioView{}, nullptr, actual);
        CHECK(std::all_of(actual.externalLoadA, actual.externalLoadA + kFrames, [](double value) { return value == 0.0; }) &&
                  flushed.status().sidechainMissing,
              "positive-frame control disconnect produces zero current and missing status");
        fresh.processBlock(parameters, context, AudioView{}, &external, expected);
        flushed.processBlock(parameters, context, AudioView{}, &external, actual);
        CHECK(std::equal(expected.externalLoadA, expected.externalLoadA + kFrames, actual.externalLoadA),
              "control reconnect after a sampled disconnect starts exactly clean");
    }
}

inline void malformedSamples() {
    const auto left = th::sine(733.0, 0.19, kRate, kFrames);
    const auto right = th::sine(1373.0, 0.11, kRate, kFrames, 0.4);
    const std::array<double, 5> malformed{
        std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(), 1e200, -std::numeric_limits<double>::max()};
    for (const auto source : {SidechainSource::External, SidechainSource::Blend}) {
        for (const int damagedChannels : {1, 2, 3}) {
            auto damagedLeft = left, damagedRight = right, cleanLeft = left, cleanRight = right;
            for (std::size_t i = 0; i < malformed.size(); ++i) {
                const std::size_t at = 5 + 37 * i;
                if (damagedChannels & 1) { damagedLeft[at] = malformed[i]; cleanLeft[at] = 0.0; }
                if (damagedChannels & 2) { damagedRight[at] = malformed[i]; cleanRight[at] = 0.0; }
            }
            SidechainLoad detector, reference, swapped, disabled;
            for (auto* current : {&detector, &reference, &swapped, &disabled}) {
                current->prepare(kRate);
                current->setBudgetAmps(kBudget);
            }
            std::vector<double> actual(kFrames), expected(kFrames), mirrored(kFrames), zero(kFrames);
            detector.process(source, 1.0, 40.0, left.data(), right.data(), true, kFrames, actual.data());
            reference.process(source, 1.0, 40.0, left.data(), right.data(), true, kFrames, expected.data());
            swapped.process(source, 1.0, 40.0, right.data(), left.data(), true, kFrames, mirrored.data());
            detector.process(source, 1.0, 40.0, damagedLeft.data(), damagedRight.data(), true, kFrames, actual.data());
            reference.process(source, 1.0, 40.0, cleanLeft.data(), cleanRight.data(), true, kFrames, expected.data());
            swapped.process(source, 1.0, 40.0, damagedRight.data(), damagedLeft.data(), true, kFrames, mirrored.data());
            CHECK(actual == expected && finiteCurrent(actual, 4.0 * kBudget),
                  "malformed samples act exactly like zero before HPF mutation, source %d channels %d", int(source), damagedChannels);
            CHECK(actual == mirrored, "malformed-input stereo linking is symmetric, source %d channels %d", int(source), damagedChannels);
            detector.process(source, 1.0, 40.0, left.data(), right.data(), true, kFrames, actual.data());
            reference.process(source, 1.0, 40.0, left.data(), right.data(), true, kFrames, expected.data());
            CHECK(actual == expected && finiteCurrent(actual, 4.0 * kBudget) &&
                      std::isfinite(detector.lastEnergyRatio()) && detector.lastEnergyRatio() > 0.0,
                  "finite suffix recovers with exactly the zero-reference history, source %d channels %d", int(source), damagedChannels);
            disabled.process(source, 0.0, 40.0, damagedLeft.data(), damagedRight.data(), true, kFrames, zero.data());
            CHECK(exactZero(zero), "depth zero is exact finite zero during malformed input");
            disabled.process(source, 0.0, 40.0, left.data(), right.data(), true, kFrames, zero.data());
            CHECK(exactZero(zero) && std::isfinite(disabled.lastEnergyRatio()) && disabled.lastEnergyRatio() > 0.0,
                  "depth-zero detector still follows later finite input without poisoned history");
        }
    }
}

inline void numericalDomain() {
    const auto loud = th::sine(1000.0, 8.0, kRate, kFrames);
    std::vector<double> output(kFrames);
    const double maximum = std::numeric_limits<double>::max();
    const double maximumBudget = maximum / 8.0;  // depth <= 2, linked energy ratio <= 4
    for (double budget : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                          -std::numeric_limits<double>::infinity(), -1.0, 0.0}) {
        SidechainLoad detector;
        detector.prepare(kRate);
        detector.setBudgetAmps(budget);
        CHECK(detector.budgetAmps() == 0.0, "nonfinite/nonpositive current budget is disabled");
        detector.process(SidechainSource::External, 2.0, 40.0, loud.data(), nullptr, true, kFrames, output.data());
        CHECK(exactZero(output), "disabled budget produces exact finite zero");
    }
    for (double budget : {kBudget, maximumBudget, maximum}) {
        SidechainLoad detector;
        detector.prepare(kRate);
        detector.setBudgetAmps(budget);
        const double admitted = std::min(budget, maximumBudget);
        CHECK(detector.budgetAmps() == admitted, "finite budget is bounded only by representable maximum current");
        detector.process(SidechainSource::Blend, 2.0, 40.0, loud.data(), loud.data(), true, kFrames, output.data());
        CHECK(finiteCurrent(output, 8.0 * admitted) && output.back() == 8.0 * admitted,
              "maximum depth and energy ratio produce the finite formula without intermediate overflow");
        detector.process(SidechainSource::Blend, 0.0, 40.0, loud.data(), loud.data(), true, kFrames, output.data());
        CHECK(exactZero(output), "depth zero remains exact with the maximum admitted budget");
    }

    // A very large, still admitted signal must be measured, not silently discarded by a final
    // finite-output mask. 2^480 is below the 2^500 detector-domain ceiling and squares safely.
    std::vector<double> large(kFrames);
    for (int i = 0; i < kFrames; ++i) large[std::size_t(i)] = i % 2 == 0 ? 0x1p480 : -0x1p480;
    for (double rate : {8000.0, 768000.0}) {
        SidechainLoad detector;
        detector.prepare(rate);
        detector.setBudgetAmps(kBudget);
        for (double hpf : {20.0, 300.0}) {
            detector.process(SidechainSource::External, 2.0, hpf, large.data(), large.data(), true, kFrames, output.data());
            CHECK(finiteCurrent(output, 8.0 * kBudget) && output.back() == 8.0 * kBudget &&
                      std::isfinite(detector.lastEnergyRatio()) && detector.lastEnergyRatio() > 1e100,
                  "large admitted signal has finite detector state at rate %g and HPF %g", rate, hpf);
            detector.process(SidechainSource::External, 0.0, hpf, large.data(), large.data(), true, kFrames, output.data());
            CHECK(exactZero(output), "large admitted samples with zero depth produce exact finite zero");
        }
    }

    const auto ordinary = th::sine(733.0, 0.19, kRate, kFrames);
    std::vector<double> expected(kFrames);
    for (double rate : {0.0, -1.0, 1.0, 7999.0, 768001.0, std::numeric_limits<double>::denorm_min(),
                        maximum, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        SidechainLoad detector, reference;
        detector.prepare(96000.0);
        detector.setBudgetAmps(kBudget);
        detector.process(SidechainSource::External, 1.0, 300.0, ordinary.data(), nullptr, true, kFrames, output.data());
        detector.prepare(rate);
        reference.prepare(kRate);
        reference.setBudgetAmps(kBudget);
        detector.process(SidechainSource::External, 1.0, 40.0, ordinary.data(), nullptr, true, kFrames, output.data());
        reference.process(SidechainSource::External, 1.0, 40.0, ordinary.data(), nullptr, true, kFrames, expected.data());
        CHECK(output == expected && detector.lastEnergyRatio() == reference.lastEnergyRatio(),
              "unsupported/nonfinite sample rate prepares a clean coherent 48 kHz detector: %g", rate);
    }
}

inline void run() {
    detectorZeroFrames();
    processorZeroFrames();
    malformedSamples();
    numericalDomain();
}
}  // namespace sidechain_regression
