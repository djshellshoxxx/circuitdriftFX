// Config model, A/B operations and 20-step undo/redo (CD-F09/F24; T09, T24 state parts).
// Synthetic data only.
#include <cmath>
#include <limits>
#include <random>
#include <string>
#include <vector>
#include "state/ABOperations.h"
#include "state/History.h"
#include "check.h"

using namespace cd;
using namespace cd::state;

namespace {
// Valid random config; every parameter and both slots' meta differ from any other draw (worst case).
Config randomConfig(std::mt19937_64& rng) {
    Config c;
    for (std::size_t i = 0; i < core::kParamCount; ++i) {
        const core::ParamSpec& s = core::kParams[i];
        if (s.kind == core::ParamKind::Float) {
            std::uniform_real_distribution<double> d(s.min, s.max);
            c.set(i, d(rng));
        } else {
            std::uniform_int_distribution<long long> d(static_cast<long long>(s.min), static_cast<long long>(s.max));
            c.set(i, double(d(rng)));
        }
    }
    for (auto& sl : c.slots) {
        sl.blueprintId.assign(kBlueprintIdMaxLen, 'a');
        for (char& ch : sl.blueprintId) ch = char('a' + rng() % 26);
        sl.seed = rng() % (kMaxSeed + 1);
    }
    return c;
}

void testConfigModel() {
    static_assert(kSlotParamCount == 39 && kGlobalParamCount + 2 * kSlotParamCount == core::kParamCount);
    Config d;
    std::string err;
    CHECK_MSG(d.validate(err), err);
    CHECK(d.toParams() == core::defaultValues());
    std::mt19937_64 rng(1);
    const Config r = randomConfig(rng);
    CHECK_MSG(r.validate(err), err);
    CHECK(Config::fromParams(r.toParams(), &r) == r);
    Config bad = r;
    bad.set(core::indexOf("mix"), 1.5);
    CHECK(!bad.validate(err));
    bad = r; bad.set(core::indexOf("slot.a.stage.activeElement"), 1.5);
    CHECK(!bad.validate(err));
    bad = r; bad.set(core::indexOf("input.gain"), std::nan(""));
    CHECK(!bad.validate(err));
    bad = r; bad.slots[1].blueprintId.clear();
    CHECK(!bad.validate(err));
    bad = r; bad.slots[0].seed = kMaxSeed + 1;
    CHECK(!bad.validate(err));
    // get/set route to the right slot.
    Config c;
    c.set(core::indexOf("slot.b.stage.drive"), 20.0);
    CHECK(c.get(core::indexOf("slot.b.stage.drive")) == 20.0);
    CHECK(c.get(core::indexOf("slot.a.stage.drive")) == 6.0);
}

void testAB() {
    std::mt19937_64 rng(7);
    Config c = randomConfig(rng);
    c.set(core::indexOf("ab.crossfade"), 0.25);
    const double x = 0.25;

    // copy A->B: B equals A (39 params + blueprint + seed); globals untouched.
    ConfigDelta d = copyAtoB(c);
    Config n = c; applyForward(n, d);
    CHECK(n.slots[1] == c.slots[0] && n.slots[0] == c.slots[0] && n.global == c.global);
    CHECK(!d.meta.empty() && d.meta[0].slot == 1);
    Config back = n; applyReverse(back, d);
    CHECK(back == c);  // one delta undoes the whole operation exactly
    CHECK(copyAtoB(n).empty());  // idempotent: a no-op yields an empty delta

    d = copyBtoA(c);
    n = c; applyForward(n, d);
    CHECK(n.slots[0] == c.slots[1] && n.slots[1] == c.slots[1] && n.global == c.global);

    // swap with mirrored crossfade.
    d = swapAB(c);
    n = c; applyForward(n, d);
    CHECK(n.slots[0] == c.slots[1] && n.slots[1] == c.slots[0]);
    CHECK(n.get(core::indexOf("ab.crossfade")) == 0.75);
    // Sound invariance: out = (1-x)*A + x*B with arbitrary slot outputs (engine states travel with the slots).
    const double sa = 0.37, sb = -0.81;
    const double before = (1.0 - x) * sa + x * sb;
    const double nx = n.get(core::indexOf("ab.crossfade"));
    const double after = (1.0 - nx) * sb + nx * sa;
    CHECK(std::fabs(before - after) <= 1e-15);
    // swap twice returns the exact original for exactly representable x; undo restores exactly anyway.
    Config twice = n; applyForward(twice, swapAB(n));
    CHECK(twice == c);
    // not mirrored: crossfade unchanged.
    n = c; applyForward(n, swapAB(c, false));
    CHECK(n.get(core::indexOf("ab.crossfade")) == x);
    // Identical slots: swap is a no-op except a mirrored crossfade; unity holds at all positions.
    Config same = c; same.slots[1] = same.slots[0];
    for (double p : {0.0, 0.3, 1.0}) CHECK(std::fabs((1.0 - p) * 0.5 + p * 0.5 - 0.5) <= 1e-16);
    CHECK(swapAB(same, false).empty());

    // Applying a delta to a plain ParamValues snapshot (adapter side).
    core::ParamValues v = c.toParams();
    applyForward(v, copyAtoB(c));
    CHECK(Config::fromParams(v, &c).slots[1].params == c.slots[0].params);
}

void testHistoryConflictReplay() {
    const auto mix = core::indexOf("mix");
    const auto gain = core::indexOf("output.gain");
    Config before;
    before.set(mix, 0.2);
    Config edited = before;
    edited.set(mix, 0.8);
    History h;
    CHECK(h.commit(before, edited, "User mix", EditKind::Numeric) == CommitResult::Recorded);

    // Host automation to an unrelated parameter survives an undo/redo roundtrip.
    Config live = edited;
    live.set(gain, 3.5);
    CHECK(h.undo(live) != nullptr);
    CHECK(live.get(mix) == 0.2 && live.get(gain) == 3.5);
    CHECK(h.redo(live) != nullptr);
    CHECK(live.get(mix) == 0.8 && live.get(gain) == 3.5);

    // A same-parameter host edit fails closed, retaining the config and both stacks.
    live.set(mix, 0.6);
    const auto undoCount = h.undoSize(), redoCount = h.redoSize(), retainedBytes = h.bytes();
    CHECK(h.undo(live) == nullptr);
    CHECK(live.get(mix) == 0.6 && h.undoSize() == undoCount &&
          h.redoSize() == redoCount && h.bytes() == retainedBytes);
    live.set(mix, 0.8);
    CHECK(h.undo(live) != nullptr && live.get(mix) == 0.2);
    live.set(mix, 0.4);
    CHECK(h.redo(live) == nullptr && live.get(mix) == 0.4 &&
          h.undoSize() == 0 && h.redoSize() == 1);
    live.set(mix, 0.2);
    CHECK(h.redo(live) != nullptr && live.get(mix) == 0.8);

    // Slot meta is checked and written at field granularity: a blueprint edit
    // must not overwrite an unrelated external seed change.
    Config bpBefore;
    Config bpAfter = bpBefore;
    bpAfter.slots[0].blueprintId = "synthetic.test";
    History blueprintHistory;
    CHECK(blueprintHistory.commit(bpBefore, bpAfter, "Blueprint", EditKind::GraphApply) ==
          CommitResult::Recorded);
    Config bpLive = bpAfter;
    bpLive.slots[0].seed = 42;
    CHECK(blueprintHistory.undo(bpLive) != nullptr);
    CHECK(bpLive.slots[0].blueprintId == bpBefore.slots[0].blueprintId && bpLive.slots[0].seed == 42);
    CHECK(blueprintHistory.redo(bpLive) != nullptr);
    CHECK(bpLive.slots[0].blueprintId == bpAfter.slots[0].blueprintId && bpLive.slots[0].seed == 42);
    bpLive.slots[0].blueprintId = "synthetic.foreign";
    CHECK(blueprintHistory.undo(bpLive) == nullptr);
    CHECK(bpLive.slots[0].blueprintId == "synthetic.foreign" && blueprintHistory.undoSize() == 1);

    // Conversely, seed-only undo must preserve external blueprint changes.
    Config seedAfter = bpBefore;
    seedAfter.slots[1].seed = 71;
    History seedHistory;
    CHECK(seedHistory.commit(bpBefore, seedAfter, "Seed", EditKind::ABOperation) ==
          CommitResult::Recorded);
    Config seedLive = seedAfter;
    seedLive.slots[1].blueprintId = "synthetic.foreign";
    CHECK(seedHistory.undo(seedLive) != nullptr);
    CHECK(seedLive.slots[1].seed == bpBefore.slots[1].seed &&
          seedLive.slots[1].blueprintId == "synthetic.foreign");

    // Defensive preflight also refuses malformed/ambiguous deltas, not just stale values.
    ConfigDelta delta = diff(before, edited);
    CHECK(canReplayDelta(before, delta, true));
    CHECK(canReplayDelta(edited, delta, false));
    ConfigDelta invalid = delta;
    invalid.params[0].index = static_cast<std::uint32_t>(core::kParamCount);
    CHECK(!canReplayDelta(before, invalid, true));
    invalid = delta;
    invalid.params.push_back(invalid.params.front());
    CHECK(!canReplayDelta(before, invalid, true));
    invalid = delta;
    invalid.params[0].after = std::numeric_limits<double>::infinity();
    CHECK(!canReplayDelta(before, invalid, true));
    invalid = delta;
    invalid.meta.push_back({2, "base.default", "synthetic.test", 0, 1});
    CHECK(!canReplayDelta(before, invalid, true));
}

void testHistoryT09() {
    std::mt19937_64 rng(42);
    constexpr int kN = 25;
    std::vector<Config> cfg;
    for (int i = 0; i <= kN; ++i) cfg.push_back(randomConfig(rng));
    History h;
    const std::string label(300, 'L');  // clipped to kMaxLabelBytes
    for (int i = 1; i <= kN; ++i) {
        const CommitResult r = h.commit(cfg[std::size_t(i - 1)], cfg[std::size_t(i)], label, EditKind::SceneLoad, kMaxTransactionBlobBytes);
        CHECK(r == CommitResult::Recorded);
        CHECK(h.bytes() <= kHistoryBudgetBytes);
    }
    CHECK(h.undoSize() == kHistoryDepth);  // 21st..25th evicted the oldest
    CHECK(h.peekUndo()->label.size() == kMaxLabelBytes);
    std::printf("  worst case: %zu transactions, %.2f MiB of %.0f MiB\n", h.undoSize(), double(h.bytes()) / 1048576.0,
                double(kHistoryBudgetBytes) / 1048576.0);
    CHECK(h.bytes() <= kHistoryBudgetBytes);

    Config cur = cfg[kN];
    for (int k = 1; k <= int(kHistoryDepth); ++k) {
        const Transaction* t = h.undo(cur);
        CHECK(t != nullptr);
        CHECK(cur == cfg[std::size_t(kN - k)]);  // canonical equality, bit-exact
    }
    CHECK(h.undo(cur) == nullptr);  // oldest was evicted: cannot go beyond 20
    CHECK(cur == cfg[kN - kHistoryDepth]);
    CHECK(h.redoSize() == kHistoryDepth && h.bytes() <= kHistoryBudgetBytes);
    for (int k = 1; k <= int(kHistoryDepth); ++k) {
        CHECK(h.redo(cur) != nullptr);
        CHECK(cur == cfg[kN - kHistoryDepth + std::size_t(k)]);
    }
    CHECK(h.redo(cur) == nullptr && cur == cfg[kN]);

    // New edit clears redo; automation neither records nor clears redo.
    h.undo(cur); h.undo(cur);
    CHECK(h.canRedo());
    Config auto1 = cur; auto1.set(core::indexOf("mix"), 0.123);
    CHECK(h.commit(cur, auto1, "host automation", EditKind::Automation) == CommitResult::ExcludedAutomation);
    CHECK(h.canRedo() && h.undoSize() == kHistoryDepth - 2);
    CHECK(!h.beginGesture(cur, "automation gesture", EditKind::Automation));
    Config edit = cur; edit.set(core::indexOf("output.gain"), 3.5);
    CHECK(h.commit(cur, edit, "Output gain", EditKind::Numeric) == CommitResult::Recorded);
    CHECK(!h.canRedo() && h.redoSize() == 0);
    CHECK(h.commit(edit, edit, "no change", EditKind::Numeric) == CommitResult::NoChange);
    h.clear();
    CHECK(!h.canUndo() && !h.canRedo() && h.bytes() == 0);
}

void testHistoryMisc() {
    std::mt19937_64 rng(3);
    const Config a = randomConfig(rng);
    // Budget rejection leaves everything unchanged.
    History tiny(1000);
    CHECK(tiny.commit(a, randomConfig(rng), "x", EditKind::Numeric) == CommitResult::RejectedBudget);
    CHECK(tiny.undoSize() == 0 && tiny.bytes() == 0);
    History h;
    CHECK(h.commit(a, randomConfig(rng), "x", EditKind::Numeric, kMaxTransactionBlobBytes + 1) == CommitResult::RejectedInvalid);

    // Gesture coalescing: many intermediate values, one transaction.
    Config cur = a;
    CHECK(h.beginGesture(cur, "Drive drag", EditKind::Gesture));
    CHECK(!h.beginGesture(cur, "nested"));
    for (int i = 1; i <= 50; ++i) cur.set(core::indexOf("slot.a.stage.drive"), double(i) * 0.5);
    CHECK(h.undo(cur) == nullptr);  // no undo while a gesture is open
    CHECK(h.endGesture(cur) == CommitResult::Recorded);
    CHECK(h.undoSize() == 1 && h.peekUndo()->delta.params.size() <= 1);
    CHECK(h.endGesture(cur) == CommitResult::RejectedInvalid);
    CHECK(h.beginGesture(cur, "noop", EditKind::Gesture));
    CHECK(h.endGesture(cur) == CommitResult::NoChange);
    h.undo(cur);
    CHECK(cur == a);

    // Wheel idle grouping (250 ms): 0, 100, 300 ms merge; 700 ms starts a new transaction.
    History w;
    Config c0 = a, c1 = a;
    const std::size_t drv = core::indexOf("slot.a.stage.drive");
    double t[4] = {0, 100, 300, 700};
    for (int i = 0; i < 4; ++i) {
        c1 = c0; c1.set(drv, c0.get(drv) + 1.0);
        w.commitWheel(c0, c1, "Drive wheel", t[i]);
        c0 = c1;
    }
    CHECK(w.undoSize() == 2);
    Config u = c0;
    w.undo(u);
    CHECK(u.get(drv) == a.get(drv) + 3.0);
    w.undo(u);
    CHECK(u == a);
    // A wheel group that nets to nothing disappears.
    History z;
    Config up = a; up.set(drv, a.get(drv) == 10.0 ? 11.0 : 10.0);
    z.commitWheel(a, up, "w", 0);
    CHECK(z.commitWheel(up, a, "w", 50) == CommitResult::Merged);
    CHECK(z.undoSize() == 0);

    // A/B transactions go through history like any other edit.
    History ab;
    Config s = a; Config s2 = applyAB(s, ABOp::Swap);
    CHECK(ab.commit(s, s2, "Swap A/B", EditKind::ABOperation) == CommitResult::Recorded);
    Config s3 = s2; ab.undo(s3);
    CHECK(s3 == s);
}

void testWheelTransactionBoundaries() {
    Config initial;
    Config first = initial;
    first.set(core::indexOf("mix"), 0.5);

    // Measure one legitimate wheel edit, then allow precisely that footprint.
    // A second edit with the same label adds metadata and must not silently
    // grow the retained group beyond the configured budget.
    History measured;
    CHECK(measured.commitWheel(initial, first, "Wheel", 0) == CommitResult::Recorded);
    History bounded(measured.bytes());
    CHECK(bounded.commitWheel(initial, first, "Wheel", 0) == CommitResult::Recorded);
    const std::size_t originalBytes = bounded.bytes();
    Config expanded = first;
    expanded.slots[0].blueprintId.assign(kBlueprintIdMaxLen, 'b');
    CHECK(bounded.commitWheel(first, expanded, "Wheel", 100) == CommitResult::RejectedBudget);
    CHECK(bounded.bytes() == originalBytes && bounded.undoSize() == 1);
    CHECK(bounded.peekUndo()->timeMs == 0 && bounded.peekUndo()->delta.meta.empty());
    Config restored = first;
    CHECK(bounded.undo(restored) != nullptr && restored == initial);
    CHECK(bounded.redo(restored) != nullptr && restored == first);

    // Undo followed by another wheel edit creates a distinct transaction.
    // One subsequent undo returns to the user's branch point.
    History branch;
    Config second = first;
    second.set(core::indexOf("mix"), 0.25);
    CHECK(branch.commitWheel(initial, first, "Wheel", 0) == CommitResult::Recorded);
    CHECK(branch.commitWheel(first, second, "Other wheel", 50) == CommitResult::Recorded);
    restored = second;
    CHECK(branch.undo(restored) != nullptr && restored == first);
    Config replacement = first;
    replacement.set(core::indexOf("mix"), 0.75);
    CHECK(branch.commitWheel(first, replacement, "Wheel", 100) == CommitResult::Recorded);
    CHECK(branch.undoSize() == 2 && !branch.canRedo());
    restored = replacement;
    CHECK(branch.undo(restored) != nullptr && restored == first);

    const std::size_t undoBefore = branch.undoSize(), redoBefore = branch.redoSize(), bytesBefore = branch.bytes();
    CHECK(branch.commitWheel(first, replacement, "Wheel", std::numeric_limits<double>::quiet_NaN()) == CommitResult::RejectedInvalid);
    CHECK(branch.undoSize() == undoBefore && branch.redoSize() == redoBefore && branch.bytes() == bytesBefore);

    // An intervening host edit to the same value also ends the wheel group.
    // Undoing the new user edit restores the host value, not the older value.
    History automation;
    CHECK(automation.commitWheel(initial, first, "Wheel", 0) == CommitResult::Recorded);
    CHECK(automation.commit(first, second, "Host", EditKind::Automation) == CommitResult::ExcludedAutomation);
    CHECK(automation.commitWheel(second, replacement, "Wheel", 100) == CommitResult::Recorded);
    restored = replacement;
    CHECK(automation.undo(restored) != nullptr && restored == second);
}
}  // namespace

int main() {
    testConfigModel();
    testAB();
    testHistoryConflictReplay();
    testHistoryT09();
    testHistoryMisc();
    testWheelTransactionBoundaries();
    return TEST_RESULT("test_history");
}
