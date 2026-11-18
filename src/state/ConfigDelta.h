// CircuitDrift state: a transaction's edit list (parameter edits + slot meta edits) and the A/B
// slot operations that produce them (CD-F24). Message/worker thread only (allocates).
//
// A/B semantics (spec 05 "State ownership and A/B"; mix law: out = (1 - x) * A + x * B, x = ab.crossfade,
// equal-sum linear):
//   copyAtoB : slot B <- slot A: all 39 slot params, blueprint id and seed. Global params, incl.
//              ab.crossfade, are untouched. (The adapter additionally starts B from a versioned
//              dynamic-state snapshot of A at a sample boundary; that is engine state, not config.)
//   copyBtoA : mirror image.
//   swap     : A <-> B for all 39 slot params, blueprint id and seed; with mirrorCrossfade (default)
//              ab.crossfade x -> 1 - x. Then (1 - x') * A' + x' * B' = x * B + (1 - x) * A: the
//              output is unchanged PROVIDED the adapter also swaps the two engines' dynamic states
//              (genuine thermal/electrical history moves with the circuit). Identical slots stay
//              unity at all positions because the law is equal-sum. 1 - x is exact for x in {0, 0.5,
//              1} and any x >= 0.5; for other x swapping twice may differ from the original x by one
//              ulp (undo restores the exact stored value, because history stores before/after).
// Every operation yields ONE ConfigDelta = one undoable transaction; no-op operations yield an
// empty delta (the caller must not push an empty transaction).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "state/Config.h"

namespace cd::state {

struct ParamEdit {
    std::uint32_t index = 0;  // registry index
    double before = 0.0, after = 0.0;
};
struct SlotMetaEdit {
    std::uint8_t slot = 0;  // 0 = A, 1 = B
    std::string beforeBlueprint, afterBlueprint;
    std::uint64_t beforeSeed = 0, afterSeed = 0;
};
struct ConfigDelta {
    std::vector<ParamEdit> params;    // ascending registry index
    std::vector<SlotMetaEdit> meta;   // ascending slot
    bool empty() const noexcept { return params.empty() && meta.empty(); }
    std::size_t bytes() const noexcept;  // heap + inline estimate
};

// Exactly the fields that differ (exact double comparison).
ConfigDelta diff(const Config& before, const Config& after);
void applyForward(Config& c, const ConfigDelta& d) noexcept;  // sets `after` values
void applyReverse(Config& c, const ConfigDelta& d) noexcept;  // sets `before` values
// Apply `after` values to a plain-value snapshot (host/adapter side); meta edits are ignored.
void applyForward(core::ParamValues& v, const ConfigDelta& d) noexcept;
// Merge `next` (which follows `last` directly) into `last`: first `before` and last `after` win;
// fields that end up unchanged are dropped.
void mergeInto(ConfigDelta& last, const ConfigDelta& next);

enum class ABOp : std::uint8_t { CopyAtoB, CopyBtoA, Swap };
Config applyAB(const Config& current, ABOp op, bool mirrorCrossfade = true);
ConfigDelta makeABDelta(const Config& current, ABOp op, bool mirrorCrossfade = true);

}  // namespace cd::state
