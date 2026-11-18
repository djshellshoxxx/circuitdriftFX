#include "state/History.h"
#include <utility>

namespace cd::state {
namespace {
constexpr std::size_t kPerTransactionOverhead = 64;  // deque node/bookkeeping allowance

std::string clipLabel(std::string_view s) {
    std::size_t n = std::min(s.size(), kMaxLabelBytes);
    while (n > 0 && n < s.size() && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;  // keep UTF-8 whole
    return std::string(s.substr(0, n));
}
}  // namespace

std::size_t Transaction::bytes() const noexcept {
    return sizeof(Transaction) + kPerTransactionOverhead + label.capacity() + delta.bytes() + blobBytes;
}

CommitResult History::record(Transaction t) {
    if (t.blobBytes > kMaxTransactionBlobBytes) return CommitResult::RejectedInvalid;
    std::size_t redoBytes = 0;
    for (const auto& r : redo_) redoBytes += r.accounted;
    std::size_t projected = bytes_ - redoBytes + t.bytes();
    if (undo_.size() >= kHistoryDepth) projected -= undo_.front().accounted;
    if (projected > budget_) return CommitResult::RejectedBudget;
    redo_.clear();
    bytes_ -= redoBytes;
    if (undo_.size() >= kHistoryDepth) { bytes_ -= undo_.front().accounted; undo_.pop_front(); }
    t.accounted = t.bytes();
    bytes_ += t.accounted;
    undo_.push_back(std::move(t));
    return CommitResult::Recorded;
}

CommitResult History::commit(const Config& before, const Config& after, std::string_view label, EditKind kind,
                             std::size_t blobBytes) {
    if (kind == EditKind::Automation) return CommitResult::ExcludedAutomation;
    Transaction t;
    t.delta = diff(before, after);
    if (t.delta.empty() && blobBytes == 0) return CommitResult::NoChange;
    t.label = clipLabel(label);
    t.kind = kind;
    t.blobBytes = blobBytes;
    return record(std::move(t));
}

CommitResult History::commitWheel(const Config& before, const Config& after, std::string_view label, double nowMs) {
    ConfigDelta d = diff(before, after);
    if (d.empty()) return CommitResult::NoChange;
    const std::string lab = clipLabel(label);
    if (!undo_.empty()) {
        Transaction& last = undo_.back();
        if (last.kind == EditKind::Wheel && last.label == lab && nowMs >= last.timeMs && nowMs - last.timeMs <= kWheelGroupMs) {
            std::size_t redoBytes = 0;
            for (const auto& r : redo_) redoBytes += r.accounted;
            bytes_ -= redoBytes;
            redo_.clear();
            bytes_ -= last.accounted;
            mergeInto(last.delta, d);
            last.timeMs = nowMs;
            if (last.delta.empty()) undo_.pop_back(); else { last.accounted = last.bytes(); bytes_ += last.accounted; }
            return CommitResult::Merged;
        }
    }
    Transaction t;
    t.delta = std::move(d);
    t.label = lab;
    t.kind = EditKind::Wheel;
    t.timeMs = nowMs;
    return record(std::move(t));
}

bool History::beginGesture(const Config& before, std::string_view label, EditKind kind) {
    if (kind == EditKind::Automation || gestureOpen_) return false;
    gestureOpen_ = true;
    gestureBefore_ = before;
    gestureLabel_ = clipLabel(label);
    gestureKind_ = kind;
    return true;
}

CommitResult History::endGesture(const Config& after) {
    if (!gestureOpen_) return CommitResult::RejectedInvalid;
    gestureOpen_ = false;
    return commit(gestureBefore_, after, gestureLabel_, gestureKind_);
}

const Transaction* History::undo(Config& cfg) {
    if (gestureOpen_ || undo_.empty()) return nullptr;
    redo_.push_back(std::move(undo_.back()));
    undo_.pop_back();
    applyReverse(cfg, redo_.back().delta);
    return &redo_.back();
}

const Transaction* History::redo(Config& cfg) {
    if (gestureOpen_ || redo_.empty()) return nullptr;
    undo_.push_back(std::move(redo_.back()));
    redo_.pop_back();
    applyForward(cfg, undo_.back().delta);
    return &undo_.back();
}

void History::clear() noexcept {
    undo_.clear();
    redo_.clear();
    bytes_ = 0;
    gestureOpen_ = false;
}

}  // namespace cd::state
