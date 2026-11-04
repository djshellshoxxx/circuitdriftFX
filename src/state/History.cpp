// CircuitDrift state: History implementation. Message thread only. See History.h.
// Owner: cd_state.
#include "state/History.h"
#include <algorithm>
#include <chrono>

namespace cd::state {

History::History(Clock clock, std::size_t budgetBytes) : clock_(std::move(clock)), budget_(std::min(budgetBytes, kBudgetBytes)) {
    current_ = makeBlob(std::string());
}

ConfigBlob History::makeBlob(std::string text) {
    text.shrink_to_fit();
    return std::make_shared<const std::string>(std::move(text));
}

std::int64_t History::now() const {
    if (clock_) return clock_();
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::size_t History::metadataBytes(const HistoryTransaction& t) noexcept {
    std::size_t n = sizeof(HistoryTransaction) + t.label.capacity() + t.affectedIds.capacity() * sizeof(std::string);
    for (const auto& id : t.affectedIds) n += id.capacity();
    return n;
}

std::size_t History::accountedBytes() const {
    std::vector<const std::string*> seen;
    seen.reserve(2 * (undo_.size() + redo_.size()) + 1);
    std::size_t bytes = sizeof(History);
    auto blob = [&](const ConfigBlob& b) {
        if (!b || std::find(seen.begin(), seen.end(), b.get()) != seen.end()) return;
        seen.push_back(b.get());
        bytes += kBlobOverhead + b->capacity();
    };
    blob(current_);
    for (const auto& t : undo_) { bytes += metadataBytes(t); blob(t.prev); blob(t.next); }
    for (const auto& t : redo_) { bytes += metadataBytes(t); blob(t.prev); blob(t.next); }
    return bytes;
}

void History::load(ConfigBlob state) {
    undo_.clear();
    redo_.clear();
    wheelGroupOpen_ = false;
    current_ = state ? std::move(state) : makeBlob(std::string());
}

CommitResult History::commit(std::string label, TxOrigin origin, std::vector<std::string> affectedIds,
                             ConfigBlob next, std::string* error) {
    auto reject = [&](const char* why) { if (error) *error = why; return CommitResult::Rejected; };
    if (!next) return reject("null document");
    if (next->size() > kMaxBlobBytes) return reject("document exceeds the 2.5 MiB session bound");
    if (label.size() > kMaxLabelBytes) return reject("label too long");
    if (affectedIds.size() > kMaxIds) return reject("too many affected IDs");
    for (const auto& id : affectedIds) if (id.size() > kMaxIdBytes) return reject("affected ID too long");

    if (origin == TxOrigin::Automation || origin == TxOrigin::External) {
        // Host remains authoritative: automation changes the live document without history.
        if (origin == TxOrigin::Automation) current_ = std::move(next);
        wheelGroupOpen_ = false;
        return CommitResult::Excluded;
    }
    if (*next == *current_) return CommitResult::NoChange;

    const std::int64_t t = now();
    if (origin == TxOrigin::Wheel && wheelGroupOpen_ && redo_.empty() && !undo_.empty()) {
        HistoryTransaction& top = undo_.back();
        if (top.origin == TxOrigin::Wheel && top.affectedIds == affectedIds && t - top.lastEditMs < kWheelIdleMs && t >= top.lastEditMs) {
            top.next = std::move(next);
            top.lastEditMs = t;
            current_ = top.next;
            if (*top.next == *top.prev) {  // gesture returned to its start: nothing to undo
                undo_.pop_back();
                wheelGroupOpen_ = false;
            }
            return CommitResult::Grouped;
        }
    }

    HistoryTransaction tx{std::move(label), origin, std::move(affectedIds), current_, std::move(next), t};
    // A single step that cannot fit even with an empty history is refused; nothing changes.
    if (sizeof(History) + metadataBytes(tx) + 2 * kBlobOverhead + tx.prev->capacity() + tx.next->capacity() > budget_)
        return reject("transaction exceeds history budget");
    redo_.clear();
    current_ = tx.next;
    undo_.push_back(std::move(tx));
    while (undo_.size() > kSteps) undo_.pop_front();
    while (undo_.size() > 1 && accountedBytes() > budget_) { undo_.pop_front(); ++evictedForBudget_; }
    wheelGroupOpen_ = origin == TxOrigin::Wheel;
    return CommitResult::Recorded;
}

ConfigBlob History::undo() {
    wheelGroupOpen_ = false;
    if (undo_.empty()) return nullptr;
    HistoryTransaction tx = std::move(undo_.back());
    undo_.pop_back();
    current_ = tx.prev;
    redo_.push_back(std::move(tx));
    return current_;
}

ConfigBlob History::redo() {
    wheelGroupOpen_ = false;
    if (redo_.empty()) return nullptr;
    HistoryTransaction tx = std::move(redo_.back());
    redo_.pop_back();
    current_ = tx.next;
    undo_.push_back(std::move(tx));
    return current_;
}

}  // namespace cd::state
