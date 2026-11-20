// Malformed checkpoint regressions. Synthetic engine fixtures, not hardware qualification.
#include <cmath>
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

    // These are the leading scalar fields of layout revision C0DE0001. Each invalid field
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
} // namespace

int main() {
    malformedCheckpointIsAtomic();
    std::printf("engine checkpoint safety: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
