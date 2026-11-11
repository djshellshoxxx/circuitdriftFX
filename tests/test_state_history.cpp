// T09 history tests: 20 worst-case (~2.5 MiB SYNTHETIC session documents) transactions
// undo/redo to canonical configuration equality, redo cleared by a new edit, automation
// excluded, wheel grouping (injected clock), project-load clearing, boundary rejections and
// the 56 MiB accounted budget. The filler timeline is synthetic, not a real timeline.
#include "state/History.h"
#include "state/StateDocument.h"
#include <cstdio>
#include <string>
#include <vector>

namespace {
using namespace cd;
using namespace cd::state;
int failures = 0;
#define CHECK(c) do { if (!(c)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

const char* kSession = R"({"schemaVersion":1,"product":"CircuitDrift","kind":"session","engineModelVersion":"0.0.1-dev",
 "minReaderVersion":1,"author":"fixture","title":"Synthetic T09 worst-case session","tags":["synthetic"],
 "createdAt":"2026-11-04T12:00:00Z","requiredFeatures":["CD-F20"],
 "blueprints":[{"schemaVersion":1,"id":"example.a","components":[{"id":"R1","modelId":"fixture.resistor"}]},
               {"schemaVersion":1,"id":"example.b","components":[{"id":"C1","modelId":"fixture.capacitor"}]}],
 "baseParameters":{"mix":1,"hpf.frequency":20.5},"macros":{},"timeline":{},"faults":[],"seed":42})";

// Builds a canonical session as close to the 2.5 MiB bound as the filler allows.
StateDocument worstCaseBase() {
    StateDocument d;
    std::string error;
    CHECK(loadState(kSession, d, error));
    const std::size_t base = saveState(d).size();
    core::JsonValue tl = core::JsonValue::makeObject();
    tl.set("synthetic", core::JsonValue::makeString("T09 filler; not a real timeline"));
    core::JsonValue pts = core::JsonValue::makeArray();
    const double sample = 0.12345678901234567;  // 19 chars + comma when written
    const std::size_t per = core::writeJson(core::JsonValue::makeNumber(sample)).size() + 1;
    const std::size_t n = (kSessionMaxBytes - base - 2048) / per;
    pts.array.assign(n, core::JsonValue::makeNumber(sample));
    tl.set("points", std::move(pts));
    d.timeline = std::move(tl);
    return d;
}

// Variant k differs in a base parameter and in timeline content (a real configuration edit).
std::string variant(StateDocument d, int k) {
    for (auto& kv : d.baseParameters) if (kv.first == "mix") kv.second = double(k) / 40.0;
    auto& pts = d.timeline.get("points")->array;
    for (int j = 0; j <= k; ++j) pts[std::size_t(j) * 997 % pts.size()] = core::JsonValue::makeNumber(0.5 + 1e-3 * j);
    return saveState(d);
}

bool canonicalEqual(const ConfigBlob& blob, const std::string& expected) {
    if (!blob || *blob != expected) return false;
    StateDocument d;
    std::string error;
    return loadState(*blob, d, error) && saveState(d) == expected;  // canonical fixed point
}

void t09WorstCase() {
    const StateDocument base = worstCaseBase();
    std::vector<std::string> states;
    for (int k = 0; k <= 21; ++k) states.push_back(variant(base, k));
    std::size_t minSize = states[0].size(), maxSize = 0;
    for (const auto& s : states) { minSize = std::min(minSize, s.size()); maxSize = std::max(maxSize, s.size()); }
    CHECK(maxSize <= kSessionMaxBytes && minSize >= kSessionMaxBytes - 8192);
    CHECK(canonicalEqual(History::makeBlob(states[0]), states[0]));

    History h;
    h.load(History::makeBlob(states[0]));
    for (int k = 1; k <= 20; ++k)
        CHECK(h.commit("edit " + std::to_string(k), TxOrigin::Gesture, {"mix"}, History::makeBlob(states[std::size_t(k)])) == CommitResult::Recorded);
    CHECK(h.undoDepth() == 20 && h.redoDepth() == 0);
    const std::size_t bytes20 = h.accountedBytes();
    CHECK(bytes20 <= History::kBudgetBytes);
    CHECK(h.evictedForBudget() == 0);
    // Automation is excluded: no history entry, redo/undo depth unchanged.
    CHECK(h.commit("automation", TxOrigin::Automation, {"mix"}, History::makeBlob(states[20])) == CommitResult::Excluded);
    CHECK(h.undoDepth() == 20);
    CHECK(h.commit("meter", TxOrigin::External, {}, History::makeBlob("meter")) == CommitResult::Excluded);
    CHECK(*h.current() == states[20]);

    bool undoOk = true;
    for (int k = 19; k >= 0; --k) {
        const ConfigBlob c = h.undo();
        if (!canonicalEqual(c, states[std::size_t(k)])) undoOk = false;
        if (h.nextRedo() == nullptr || h.nextRedo()->label != "edit " + std::to_string(k + 1)) undoOk = false;
    }
    CHECK(undoOk);
    CHECK(h.undo() == nullptr && h.undoDepth() == 0 && h.redoDepth() == 20);
    bool redoOk = true;
    for (int k = 1; k <= 20; ++k) {
        const ConfigBlob c = h.redo();
        if (!c || *c != states[std::size_t(k)]) redoOk = false;
        const HistoryTransaction* t = h.nextUndo();
        if (!t || t->origin != TxOrigin::Gesture || t->affectedIds != std::vector<std::string>{"mix"} ||
            *t->prev != states[std::size_t(k - 1)] || *t->next != states[std::size_t(k)]) redoOk = false;
    }
    CHECK(redoOk);
    CHECK(canonicalEqual(h.current(), states[20]));
    CHECK(h.redo() == nullptr);
    // New edit clears redo.
    h.undo(); h.undo(); h.undo();
    CHECK(h.redoDepth() == 3);
    CHECK(h.commit("new edit", TxOrigin::Gesture, {"hpf.frequency"}, History::makeBlob(states[21])) == CommitResult::Recorded);
    CHECK(h.redoDepth() == 0 && h.redo() == nullptr && h.undoDepth() == 18);
    // Overfill: the oldest step is evicted, at least 20 retained, budget respected.
    for (int k = 0; k < 4; ++k)
        CHECK(h.commit("more", TxOrigin::Gesture, {"mix"}, History::makeBlob(states[std::size_t(k % 2 ? 21 : 5)])) == CommitResult::Recorded);
    CHECK(h.undoDepth() == 20);
    CHECK(h.accountedBytes() <= History::kBudgetBytes);
    int undone = 0;
    while (h.undo()) ++undone;
    CHECK(undone == 20 && *h.current() != states[0]);
    std::printf("T09: 20 worst-case transactions of %.3f-%.3f MiB; accounted history %.2f MiB (cap 56 MiB); undo/redo canonical equality %s\n",
                double(minSize) / 1048576.0, double(maxSize) / 1048576.0, double(bytes20) / 1048576.0,
                undoOk && redoOk ? "exact" : "FAILED");
    // Project load clears both stacks.
    h.redo();
    h.load(History::makeBlob(states[3]));
    CHECK(h.undoDepth() == 0 && h.redoDepth() == 0 && *h.current() == states[3]);
}

void wheelGrouping() {
    std::int64_t now = 0;
    History h([&] { return now; });
    h.load(History::makeBlob("s0"));
    CHECK(h.commit("wheel", TxOrigin::Wheel, {"mix"}, History::makeBlob("s1")) == CommitResult::Recorded);
    now = 100; CHECK(h.commit("wheel", TxOrigin::Wheel, {"mix"}, History::makeBlob("s2")) == CommitResult::Grouped);
    now = 349; CHECK(h.commit("wheel", TxOrigin::Wheel, {"mix"}, History::makeBlob("s3")) == CommitResult::Grouped);
    now = 599; CHECK(h.commit("wheel", TxOrigin::Wheel, {"mix"}, History::makeBlob("s4")) == CommitResult::Recorded);  // 250 ms idle
    now = 650; CHECK(h.commit("wheel", TxOrigin::Wheel, {"hpf.frequency"}, History::makeBlob("s5")) == CommitResult::Recorded);  // other ID
    now = 700; CHECK(h.commit("drag", TxOrigin::Gesture, {"hpf.frequency"}, History::makeBlob("s6")) == CommitResult::Recorded);
    now = 720; CHECK(h.commit("wheel", TxOrigin::Wheel, {"hpf.frequency"}, History::makeBlob("s7")) == CommitResult::Recorded);  // gesture closed group
    CHECK(h.undoDepth() == 5);
    CHECK(*h.undo() == "s6" && *h.undo() == "s5" && *h.undo() == "s4" && *h.undo() == "s3" && *h.undo() == "s0");
    // Undo ends a group: the next wheel tick is a new transaction even within 250 ms.
    h.load(History::makeBlob("w0"));
    now = 1000; h.commit("wheel", TxOrigin::Wheel, {"mix"}, History::makeBlob("w1"));
    now = 1010; CHECK(*h.undo() == "w0");
    now = 1020; CHECK(h.commit("wheel", TxOrigin::Wheel, {"mix"}, History::makeBlob("w2")) == CommitResult::Recorded);
    // Wheel returning to its start inside one group leaves nothing to undo.
    now = 1030; CHECK(h.commit("wheel", TxOrigin::Wheel, {"mix"}, History::makeBlob("w0")) == CommitResult::Grouped);
    CHECK(h.undoDepth() == 0 && *h.current() == "w0");
    // Clock going backwards never groups.
    now = 2000; h.commit("wheel", TxOrigin::Wheel, {"mix"}, History::makeBlob("w3"));
    now = 1500; CHECK(h.commit("wheel", TxOrigin::Wheel, {"mix"}, History::makeBlob("w4")) == CommitResult::Recorded);
}

void rejectionsAndBudget() {
    History h([] { return std::int64_t(0); });
    h.load(History::makeBlob("a"));
    std::string error;
    CHECK(h.commit("x", TxOrigin::Gesture, {}, nullptr, &error) == CommitResult::Rejected && !error.empty());
    CHECK(h.commit("x", TxOrigin::Gesture, {}, History::makeBlob(std::string(kSessionMaxBytes + 1, ' ')), &error) == CommitResult::Rejected);
    CHECK(h.commit(std::string(300, 'l'), TxOrigin::Gesture, {}, History::makeBlob("b")) == CommitResult::Rejected);
    CHECK(h.commit("x", TxOrigin::Gesture, std::vector<std::string>(65, "id"), History::makeBlob("b")) == CommitResult::Rejected);
    CHECK(h.commit("x", TxOrigin::Gesture, {std::string(129, 'i')}, History::makeBlob("b")) == CommitResult::Rejected);
    CHECK(h.commit("same", TxOrigin::Gesture, {}, History::makeBlob("a")) == CommitResult::NoChange);
    CHECK(h.undoDepth() == 0 && *h.current() == "a");
    // Scene load and A/B copy/swap are ordinary single transactions.
    CHECK(h.commit("Load scene", TxOrigin::SceneLoad, {"slot.a"}, History::makeBlob("scene")) == CommitResult::Recorded);
    CHECK(h.commit("A/B swap", TxOrigin::AbOperation, {"slot.a", "slot.b"}, History::makeBlob("swapped")) == CommitResult::Recorded);
    CHECK(h.undoDepth() == 2);

    // A smaller configured budget evicts the oldest transactions but never exceeds its cap.
    History small([] { return std::int64_t(0); }, 8u * 1024u * 1024u);
    small.load(History::makeBlob(std::string(2000000, '0')));
    for (int k = 1; k <= 10; ++k)
        CHECK(small.commit("big", TxOrigin::Gesture, {}, History::makeBlob(std::string(2000000, char('0' + k)))) == CommitResult::Recorded);
    CHECK(small.accountedBytes() <= small.budgetBytes());
    CHECK(small.undoDepth() == 3 && small.evictedForBudget() == 7);
    History tiny([] { return std::int64_t(0); }, 1024u * 1024u);
    tiny.load(History::makeBlob("x"));
    CHECK(tiny.commit("huge", TxOrigin::Gesture, {}, History::makeBlob(std::string(2000000, 'h'))) == CommitResult::Rejected);
    CHECK(*tiny.current() == "x" && tiny.undoDepth() == 0);  // live configuration unchanged on rejection
    History capped([] { return std::int64_t(0); }, std::size_t(1) << 40);
    CHECK(capped.budgetBytes() == History::kBudgetBytes);    // hard cap cannot be raised
}

}  // namespace

int main() {
    t09WorstCase();
    wheelGrouping();
    rejectionsAndBudget();
    if (failures) { std::printf("state_history: %d failure(s)\n", failures); return 1; }
    std::puts("state_history: all checks passed (synthetic documents)");
    return 0;
}
