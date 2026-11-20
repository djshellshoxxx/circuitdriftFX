// Control boundary regressions. All fixtures are synthetic; no physical validation is claimed.
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include "TestHarness.h"
#include "control/ControlProcessor.h"
#include "control/CreativeDrift.h"

using namespace cd;
using namespace cd::control;

namespace {

bool sameState(const CreativeDrift::State& a, const CreativeDrift::State& b) {
    if (a.samples != b.samples || a.ticks != b.ticks || a.q != b.q || a.delayUnitMs != b.delayUnitMs) return false;
    for (int j = 0; j < 3; ++j) {
        if (a.u[j] != b.u[j] || a.rng[j].spare != b.rng[j].spare || a.rng[j].hasSpare != b.rng[j].hasSpare) return false;
        for (int i = 0; i < 4; ++i) if (a.rng[j].s[i] != b.rng[j].s[i]) return false;
    }
    return true;
}

void rejectedDriftSnapshots() {
    CreativeDrift drift, reference;
    drift.prepare(48000, 1234, 0);
    reference.prepare(48000, 1234, 0);
    drift.advance(12345);
    reference.advance(12345);
    const auto before = drift.save();
    auto reject = [&](CreativeDrift::State bad, const char* label) {
        drift.restore(bad);
        CHECK(sameState(drift.save(), before), "invalid snapshot ignored transactionally: %s", label);
        // Keep an unfixed implementation from executing a malicious historical tick backlog.
        drift.restore(before);
    };
    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    constexpr double inf = std::numeric_limits<double>::infinity();
    auto bad = before; bad.q = nan; reject(bad, "NaN output");
    bad = before; bad.q = 1.1; reject(bad, "output range");
    bad = before; bad.q = -before.q; reject(bad, "output inconsistent with processes");
    bad = before; bad.delayUnitMs = inf; reject(bad, "infinite delay");
    bad = before; bad.delayUnitMs = -1.6; reject(bad, "delay range");
    for (int j = 0; j < 3; ++j) {
        bad = before; bad.u[j] = nan; reject(bad, "NaN process");
        bad = before; bad.u[j] = std::numeric_limits<double>::max(); reject(bad, "unreachable process magnitude");
        bad = before; bad.rng[j].spare = inf; reject(bad, "infinite Gaussian spare");
        bad = before; bad.rng[j].spare = 1e300; reject(bad, "unreachable Gaussian spare");
        bad = before; bad.rng[j].hasSpare = 2; reject(bad, "invalid Gaussian flag");
        bad = before; for (auto& word : bad.rng[j].s) word = 0; reject(bad, "zero PRNG state");
    }
    bad = before; bad.ticks = 0; reject(bad, "historical tick backlog");
    bad = before; ++bad.ticks; reject(bad, "clock ahead of samples");
    bad = before; bad.samples = std::numeric_limits<uint64_t>::max(); reject(bad, "huge sample backlog");
    for (int i = 0; i < 20; ++i) {
        drift.advance(777);
        reference.advance(777);
        CHECK(sameState(drift.save(), reference.save()), "rejected restore preserves future RNG sequence");
    }
}

// These explicit rational rates let the oracle use small products, independently of production
// clock arithmetic. The second pair has a non-integer supported sample rate.
struct Rate { double hz; uint64_t period, step; };
constexpr Rate kRates[] = {{8000, 80, 1}, {44100, 441, 1}, {44100.5, 88201, 200},
                          {48000, 480, 1}, {88200, 882, 1}, {192000, 1920, 1}, {768000, 7680, 1}};

uint64_t expectedTicks(uint64_t samples, Rate rate) {
    return (samples / rate.period) * rate.step + (samples % rate.period) * rate.step / rate.period;
}

void exactAndBoundedDriftClock() {
    for (const auto rate : kRates) {
        CreativeDrift whole, split, restored;
        whole.prepare(rate.hz, 99, 1);
        split.prepare(rate.hz, 99, 1);
        restored.prepare(rate.hz, 888, 0);
        whole.advance(200003);
        for (int left = 200003; left > 0;) {
            const int frames = std::min(left, 17);
            split.advance(frames);
            left -= frames;
        }
        CHECK(sameState(whole.save(), split.save()), "partition-independent drift at %.9g Hz", rate.hz);
        CHECK(whole.ticks() == expectedTicks(200003, rate), "exact 100 Hz tick count at %.9g Hz", rate.hz);
        restored.restore(split.save());
        for (int i = 0; i < 25; ++i) {
            split.advance(127);
            restored.advance(127);
            CHECK(sameState(split.save(), restored.save()), "fractional-rate checkpoint continuation at %.9g Hz", rate.hz);
        }

        // Synthetic counter-boundary fixture: keep finite process/RNG values and move only the
        // coherent clock near its storage limit. One frame must never request historical work.
        auto nearLimit = whole.save();
        nearLimit.samples = std::numeric_limits<uint64_t>::max() - 1000;
        nearLimit.ticks = expectedTicks(nearLimit.samples, rate);
        whole.restore(nearLimit);
        split.restore(nearLimit);
        CHECK(sameState(whole.save(), nearLimit), "coherent large clock accepted at %.9g Hz", rate.hz);
        whole.advance(2000);
        for (int i = 0; i < 2000; ++i) {
            const auto ticks = split.ticks();
            split.advance(1);
            CHECK(split.ticks() >= ticks && split.ticks() - ticks <= 1, "one-frame tick work bounded at %.9g Hz", rate.hz);
        }
        CHECK(sameState(whole.save(), split.save()), "counter limit independent of partition at %.9g Hz", rate.hz);
        CHECK(whole.save().samples == std::numeric_limits<uint64_t>::max(), "sample clock saturates at %.9g Hz", rate.hz);
        CHECK(whole.ticks() == expectedTicks(whole.save().samples, rate), "saturated clock remains coherent at %.9g Hz", rate.hz);
        const auto held = whole.save();
        whole.advance(1);
        whole.advance(0);
        whole.advance(-1);
        CHECK(sameState(whole.save(), held), "saturated or nonpositive advance holds state");
    }

    // A full-significand rate just above 44.1 kHz must not round the 100th tick
    // back onto sample 44100. Its exact clock period is 44100*2^37+1, step 100*2^37.
    const double rate = std::nextafter(44100.0, 50000.0);
    CreativeDrift whole, split, replay;
    whole.prepare(rate, 77, 0); split.prepare(rate, 77, 0); replay.prepare(rate, 1, 1);
    whole.advance(44100);
    for (int i = 0; i < 44100; ++i) split.advance(1);
    CHECK(whole.ticks() == 99 && sameState(whole.save(), split.save()), "full-significand sample rate preserves exact boundary");
    replay.restore(whole.save());
    whole.advance(1); replay.advance(1);
    CHECK(whole.ticks() == 100 && sameState(whole.save(), replay.save()), "fractional boundary survives checkpoint restore");
    constexpr uint64_t scale = uint64_t(1) << 37;
    auto boundary = whole.save();
    boundary.samples = 44100 * scale;
    boundary.ticks = 100 * scale - 1;
    whole.restore(boundary);
    CHECK(sameState(whole.save(), boundary), "large full-significand clock accepted");
    whole.advance(1);
    CHECK(whole.save().samples == boundary.samples + 1 && whole.ticks() == boundary.ticks + 1,
          "large full-significand clock advances exactly one tick");
}

void safeParameterConversion() {
    constexpr std::array<const char*, 6> ids = {"tempo.division", "tempo.modifier", "key.source",
                                               "key.scale", "key.root", "sidechain.source"};
    constexpr std::array<double, 6> maxima = {5, 2, 2, 5, 11, 2};
    for (double input : {std::numeric_limits<double>::max(), -std::numeric_limits<double>::max(),
                         4294967297.0, -4294967297.0, std::numeric_limits<double>::quiet_NaN(),
                         std::numeric_limits<double>::infinity()}) {
        ControlProcessor actual, expected;
        actual.prepare(48000, 480, 123);
        expected.prepare(48000, 480, 123);
        auto p = core::defaultValues();
        p[core::indexOf("key.enabled")] = 1;
        p[core::indexOf("tempo.enabled")] = 1;
        p[core::indexOf("tempo.depth")] = 1;
        auto q = p;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            p[core::indexOf(ids[i])] = input;
            q[core::indexOf(ids[i])] = std::isfinite(input) && input > 0 ? maxima[i] : 0;
        }
        p[core::indexOf("key.mask")] = input;
        q[core::indexOf("key.mask")] = !std::isfinite(input) || input > 0 ? 4095 : 0;
        ControlContext ctx; ctx.frames = 480;
        core::ModulationBlock a, b;
        actual.processBlock(p, ctx, {}, nullptr, a);
        expected.processBlock(q, ctx, {}, nullptr, b);
        for (int s = 0; s < 2; ++s) {
            CHECK(a.slot[s].sidechainMode == b.slot[s].sidechainMode, "sidechain enum clamps before conversion");
            CHECK(a.slot[s].recoveryTauS == b.slot[s].recoveryTauS, "tempo enum clamps before conversion");
            CHECK(a.slot[s].creativeCutoffOct == b.slot[s].creativeCutoffOct, "scale/root/mask clamp before conversion");
        }
    }

    // The custom mask keeps truncation semantics for in-range fractional inputs.
    ControlProcessor a, b;
    a.prepare(48000, 480, 17); b.prepare(48000, 480, 17);
    auto p = core::defaultValues();
    p[core::indexOf("key.enabled")] = 1;
    p[core::indexOf("key.scale")] = 5;
    p[core::indexOf("key.mask")] = 1.9;
    auto q = p; q[core::indexOf("key.mask")] = 1;
    ControlContext ctx; ctx.frames = 480;
    core::ModulationBlock ma, mb;
    a.processBlock(p, ctx, {}, nullptr, ma); b.processBlock(q, ctx, {}, nullptr, mb);
    CHECK(ma.slot[0].creativeCutoffOct == mb.slot[0].creativeCutoffOct, "fractional custom mask still truncates");
}

void deterministicReset() {
    ControlProcessor used, fresh;
    used.prepare(44100, 127, 0x12345678);
    fresh.prepare(44100, 127, 0x12345678);
    auto p = core::defaultValues();
    p[core::indexOf("macro.drift")] = 1;
    p[core::indexOf("pitchDrift.enabled")] = 1;
    p[core::indexOf("tempo.enabled")] = 1;
    p[core::indexOf("tempo.depth")] = 1;
    p[core::indexOf("key.source")] = 1;
    p[core::indexOf("key.enabled")] = 1;
    ControlContext ctx; ctx.frames = 127;
    core::ModulationBlock a, b;
    for (int i = 0; i < 100; ++i) used.processBlock(p, ctx, {}, nullptr, a);
    used.reset();
    for (int s = 0; s < 2; ++s) CHECK(sameState(used.drift(s).save(), fresh.drift(s).save()), "reset restores initial seeded drift");
    ctx.frames = 0;
    used.processBlock(p, ctx, {}, nullptr, a); fresh.processBlock(p, ctx, {}, nullptr, b);
    CHECK(a.slot[0].creativeCutoffOct == b.slot[0].creativeCutoffOct && used.status().tempoPhase == fresh.status().tempoPhase,
          "zero-frame output after reset matches fresh preparation");
    for (int i = 0; i < 80; ++i) {
        ctx.frames = i % 2 ? 127 : 17;
        p[core::indexOf("key.enabled")] = i < 20;
        used.processBlock(p, ctx, {}, nullptr, a); fresh.processBlock(p, ctx, {}, nullptr, b);
        for (int s = 0; s < 2; ++s) {
            CHECK(sameState(used.drift(s).save(), fresh.drift(s).save()), "reset reproduces future RNG state");
            CHECK(a.slot[s].creativeCutoffOct == b.slot[s].creativeCutoffOct &&
                  a.slot[s].creativeDelayMs == b.slot[s].creativeDelayMs && a.slot[s].recoveryTauS == b.slot[s].recoveryTauS,
                  "reset reproduces future modulation");
        }
    }
    // Re-preparing updates the seed used by subsequent resets.
    used.prepare(48000, 127, 9); fresh.prepare(48000, 127, 9);
    used.processBlock(p, ctx, {}, nullptr, a);
    used.reset();
    CHECK(sameState(used.drift(0).save(), fresh.drift(0).save()), "reset uses the most recent prepare seed");
}

void invalidRatesFallback() {
    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    constexpr double inf = std::numeric_limits<double>::infinity();
    for (double rate : {nan, inf, -inf, 0.0, -1.0, 7999.0, 768001.0, std::numeric_limits<double>::max(),
                        std::numeric_limits<double>::denorm_min()}) {
        CreativeDrift actual, expected;
        actual.prepare(rate, 5, 0); expected.prepare(48000, 5, 0);
        actual.advance(1000); expected.advance(1000);
        CHECK(sameState(actual.save(), expected.save()), "unsupported drift sample rate uses 48 kHz");
        ControlProcessor cp;
        cp.prepare(rate, 17, 5);
        auto p = core::defaultValues();
        p[core::indexOf("macro.drift")] = 1;
        ControlContext ctx; ctx.frames = 17;
        core::ModulationBlock m;
        cp.processBlock(p, ctx, {}, nullptr, m);
        CHECK(std::isfinite(m.slot[0].creativeCutoffOct) && std::isfinite(cp.status().referenceNote), "invalid preparation rate stays finite");
    }
}

void referenceTuningBoundary() {
    auto p = core::defaultValues();
    p[core::indexOf("key.enabled")] = 1;
    ControlContext ctx; ctx.frames = 480;
    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    constexpr double inf = std::numeric_limits<double>::infinity();
    for (double tuning : {nan, inf, -inf, 0.0, -1.0}) {
        ControlProcessor actual, reference;
        actual.prepare(48000, 480, 11); reference.prepare(48000, 480, 11);
        actual.setReferenceTuning(tuning);
        core::ModulationBlock a, b;
        actual.processBlock(p, ctx, {}, nullptr, a); reference.processBlock(p, ctx, {}, nullptr, b);
        CHECK(actual.status().referenceNote == reference.status().referenceNote &&
              a.slot[0].creativeCutoffOct == b.slot[0].creativeCutoffOct,
              "invalid reference tuning uses 440 Hz before pitch quantization");
    }
    ControlProcessor cp;
    cp.prepare(48000, 480, 11);
    for (double tuning : {std::numeric_limits<double>::denorm_min(), std::numeric_limits<double>::min(),
                           std::numeric_limits<double>::max(), 1.0, 432.0, 440.0}) {
        cp.setReferenceTuning(tuning);
        core::ModulationBlock m;
        cp.processBlock(p, ctx, {}, nullptr, m);
        CHECK(std::isfinite(cp.status().referenceNote) && std::isfinite(m.slot[0].creativeCutoffOct),
              "positive finite tuning never overflows the pitch ratio");
    }
    cp.setReferenceTuning(432.0);
    core::ModulationBlock m;
    cp.processBlock(p, ctx, {}, nullptr, m);
    const double f = p[core::indexOf("slot.a.filter.lpfFrequency")];
    CHECK(std::fabs(cp.status().referenceNote - scale::hzToMidi(f, 432.0)) < 1e-12,
          "ordinary alternate tuning keeps its pitch mapping");
}

void transportBoundary() {
    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    constexpr double inf = std::numeric_limits<double>::infinity();
    for (double value : {nan, inf, -inf}) {
        TempoBreathing actual, reference;
        actual.prepare(48000); reference.prepare(48000);
        TempoInput input;
        input.enabled = true; input.depth = 1; input.bpm = 90; input.ppq = 2.125;
        actual.process(input, 480); reference.process(input, 480);
        input.ppq = value; input.bpm = value;
        auto expectedInput = input; expectedInput.ppq.reset(); expectedInput.bpm.reset();
        const auto a = actual.process(input, 480);
        const auto b = reference.process(expectedInput, 480);
        CHECK(a.fallback && a.bpmUsed == 120 && a.phase == b.phase && a.cutoffOct == b.cutoffOct &&
              a.recoveryTauS == b.recoveryTauS, "invalid transport values behave as unavailable");
        input.ppq = 2.75; input.bpm = 90;
        const auto recovered = actual.process(input, 480);
        CHECK(std::isfinite(recovered.phase) && std::isfinite(recovered.cutoffOct), "valid transport recovers without poisoned phase");
    }
    for (double ppq : {std::numeric_limits<double>::max(), -std::numeric_limits<double>::max()}) {
        TempoBreathing tempo;
        tempo.prepare(48000);
        TempoInput input;
        input.enabled = true; input.depth = 1; input.bpm = 120; input.ppq = ppq;
        input.division = 0; input.modifier = 2; input.playing = false;
        const auto out = tempo.process(input, 480);
        CHECK(std::isfinite(out.phase) && out.phase >= 0 && out.phase < 1 &&
              std::isfinite(out.cutoffOct) && std::fabs(out.cutoffOct) <= 0.15,
              "finite extreme PPQ is reduced before cycle division");
    }
    TempoBreathing negative;
    negative.prepare(48000);
    TempoInput input;
    input.enabled = true; input.depth = 1; input.bpm = 120; input.ppq = -0.03125;
    input.division = 0; input.playing = false;
    CHECK(negative.process(input, 480).phase == 0.75, "negative PPQ retains wrapped cycle phase");
}

}  // namespace

int main() {
    rejectedDriftSnapshots();
    safeParameterConversion();
    deterministicReset();
    exactAndBoundedDriftClock();
    invalidRatesFallback();
    referenceTuningBoundary();
    transportBoundary();
    if (g_fail) { std::printf("control safety: %d failure(s)\n", g_fail); return 1; }
    std::printf("control safety: all passed\n");
    return 0;
}
