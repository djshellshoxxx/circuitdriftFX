// Malformed checkpoint regressions. Synthetic engine fixtures, not hardware qualification.
#include <cmath>
#include <array>
#include <bit>
#include <cstdio>
#include <limits>
#include "core/ParameterRegistry.h"
#include "engine/CircuitEngine.h"

namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (false)

void malformedCheckpointIsAtomic() {
    cd::engine::CircuitEngine engine;
    cd::engine::EngineConfig cfg;
    cfg.channels = 1;
    auto params = cd::core::defaultValues();
    std::string error;
    CHECK(engine.prepare(cfg, params, error));
    cd::engine::Checkpoint baseline, after;
    engine.allocateCheckpoint(baseline);
    engine.allocateCheckpoint(after);
    CHECK(engine.getCheckpoint(baseline));
    CHECK(engine.setCheckpoint(baseline));

    // These are the leading scalar fields of the unchanged checkpoint shape. Each invalid field
    // used to pass the all-finite check and could select an invalid array or delay position.
    struct BadScalar { std::size_t at; double value; };
    for (const BadScalar bad : {
             BadScalar{1, 2},       // boolean
             BadScalar{2, 2},       // active lane index
             BadScalar{2, -1},
             BadScalar{3, 2},       // quality state
             BadScalar{4, -1},      // quality transition position
             BadScalar{5, 5},       // oversampling bank index
             BadScalar{5, -1},
             BadScalar{6, 0},       // Newton iteration cap
             BadScalar{9, -1},      // consecutive solver failures
             BadScalar{10, -1},     // debug injection count
             BadScalar{19, -0.1},   // HPF enable ramp
             BadScalar{20, 1.1},    // soft-clip enable ramp
             BadScalar{25, -1},     // gain matcher dry energy
             BadScalar{26, -1},     // gain matcher wet energy
             BadScalar{27, 1000},   // log gain outside the 0.25..4 gain contract
         }) {
        auto cp = baseline;
        cp.values[bad.at] = bad.value;
        CHECK(!engine.setCheckpoint(cp));
        CHECK(engine.getCheckpoint(after));
        CHECK(after.values == baseline.values && after.rng == baseline.rng);
        CHECK(engine.setCheckpoint(baseline));
    }

    // A late rejection must not leave earlier valid fields applied to the live engine.
    auto cp = baseline;
    cp.values[1] = 1; // a valid epoch-seen flag, visited before this invalid integer
    cp.values[9] = 0.5;
    CHECK(!engine.setCheckpoint(cp));
    CHECK(engine.getCheckpoint(after));
    CHECK(after.values == baseline.values && after.rng == baseline.rng);
    CHECK(engine.setCheckpoint(baseline));

    // Rejected input stays rejected before any float-to-integer conversion can overflow.
    cp = baseline;
    cp.values[2] = std::numeric_limits<double>::max();
    CHECK(!engine.setCheckpoint(cp));
    CHECK(engine.getCheckpoint(after));
    CHECK(after.values == baseline.values && after.rng == baseline.rng);
}
void legacyAlgorithmIdentityIsRejected() {
    cd::engine::EngineConfig cfg;
    cfg.sampleRate = 8000; cfg.channels = 1; cfg.maxBlock = 64;
    auto params = cd::core::defaultValues();
    params[cd::core::indexOf("quality.zeroLatency")] = 0;
    params[cd::core::indexOf("quality.phase")] = 0;
    params[cd::core::indexOf("pitchDrift.enabled")] = 0;
    params[cd::core::indexOf("slot.a.filter.lpfEnabled")] = 1;
    params[cd::core::indexOf("slot.a.filter.lpfFrequency")] = 20000;
    cd::engine::CircuitEngine engine;
    std::string error;
    CHECK(engine.prepare(cfg, params, error));
    if (!engine.isPrepared()) return;
    std::array<double, 64> in{}, out{};
    const double* inputs[] = {in.data()};
    double* outputs[] = {out.data()};
    cd::core::ModulationBlock modulation;
    modulation.slot[0].creativeCutoffOct = -2;
    cd::engine::ProcessContext context;
    context.sampleRate = cfg.sampleRate; context.frames = 64;
    engine.process(context, inputs, outputs, 1, nullptr, 0, 64, params, modulation);
    cd::engine::Checkpoint before, after;
    engine.allocateCheckpoint(before); engine.allocateCheckpoint(after);
    CHECK(engine.getCheckpoint(before));
    CHECK(engine.setCheckpoint(before));
    // Freeze the prior identity calculation independently of production's
    // revision. This is legacy-ID admission with an owned current-shape record,
    // not state transferred from a historical binary.
    uint64_t legacyId = 1469598103934665603ull;
    const auto mix = [&](uint64_t v) { legacyId ^= v; legacyId *= 1099511628211ull; };
    mix(std::bit_cast<uint64_t>(cfg.sampleRate));
    mix(uint64_t(cfg.channels)); mix(0); mix(0); mix(0);
    mix(uint64_t(engine.latencySamples()));
    mix(cfg.seed); mix(cfg.variationSeed[0]); mix(cfg.variationSeed[1]);
    mix(0xC0DE0001ull);
    auto legacy = before;
    legacy.layout = legacyId;
    legacy.values[1] = before.values[1] == 0.0 ? 1.0 : 0.0;
    std::printf("checkpoint algorithm identity: legacy=%llx current=%llx\n",
                static_cast<unsigned long long>(legacyId), static_cast<unsigned long long>(before.layout));
    CHECK(!engine.setCheckpoint(legacy));
    CHECK(engine.getCheckpoint(after));
    CHECK(after.values == before.values && after.rng == before.rng &&
          after.sampleIndex == before.sampleIndex && after.layout == before.layout);
}
} // namespace

int main() {
    malformedCheckpointIsAtomic();
    legacyAlgorithmIdentityIsRejected();
    std::printf("engine checkpoint safety: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
