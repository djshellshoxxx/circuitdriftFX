// CircuitDrift state: 20-step undo/redo of completed configuration transactions (CD-F09, T09).
// MESSAGE THREAD ONLY (allocates; not thread-safe; never call from audio).
//
// A transaction = one completed gesture / wheel group / numeric edit / scene load / A/B op / macro
// change: the ConfigDelta (per-parameter before/after + slot meta), label, kind, and the byte size of
// any shared immutable blob (graph) it pins. Undo/redo move the Config to the exact before/after
// values, so they reach canonical Config equality. They never touch the physics clock.
//
// Excluded: EditKind::Automation (host-driven) is never recorded and does NOT clear redo.
// A new recorded edit clears redo. Retention: the newest kDepth (20) transactions; the 21st evicts
// the oldest. Byte accounting covers per-transaction inline + heap bytes + pinned blob bytes. The hard
// budget is kHistoryBudgetBytes (56 MiB; 20 retained transactions each pinning a worst-case 2.5 MiB
// blob = 50 MiB + metadata). A commit that would exceed the budget even after evicting down to the
// minimum retention (kDepth) is REJECTED (CommitResult::RejectedBudget) and changes nothing.
// A project load calls clear().
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include "state/ConfigDelta.h"

namespace cd::state {

enum class EditKind : std::uint8_t {
    Gesture, Wheel, Numeric, SceneLoad, ABOperation, MacroChange, GraphApply, FaultApply, TimelineScrub,
    Automation  // excluded from history
};

inline constexpr std::size_t kHistoryDepth = 20;
inline constexpr std::size_t kHistoryBudgetBytes = 56u * 1024u * 1024u;
inline constexpr std::size_t kMaxTransactionBlobBytes = 2621440;  // one pinned 2.5 MiB session blob
inline constexpr std::size_t kMaxLabelBytes = 128;
inline constexpr double kWheelGroupMs = 250.0;

struct Transaction {
    std::string label;
    EditKind kind = EditKind::Numeric;
    ConfigDelta delta;
    std::size_t blobBytes = 0;  // shared immutable blob(s) pinned by this step (not owned here)
    double timeMs = 0.0;        // wheel idle grouping clock (caller's monotonic ms)
    std::size_t bytes() const noexcept;
    std::size_t accounted = 0;  // bytes charged to the history total when recorded
};

enum class CommitResult : std::uint8_t {
    Recorded, Merged, NoChange, ExcludedAutomation, RejectedBudget, RejectedInvalid
};

class History {
public:
    explicit History(std::size_t budgetBytes = kHistoryBudgetBytes) : budget_(budgetBytes) {}

    // One completed edit: `before` -> `after`. `blobBytes` = size of pinned shared blobs.
    CommitResult commit(const Config& before, const Config& after, std::string_view label, EditKind kind,
                        std::size_t blobBytes = 0);
    // Wheel gesture: merges into the previous Wheel transaction with the same label when the gap since
    // its last event is <= kWheelGroupMs, its edited values still match `before`, and no undo branch
    // is pending. Non-finite timestamps are rejected. A merge that exceeds the hard byte budget
    // leaves the original transaction unchanged.
    CommitResult commitWheel(const Config& before, const Config& after, std::string_view label, double nowMs);

    // Mouse drag / continuous gesture: begin with the config at pointer-down, end with the config at
    // release. One transaction, however many intermediate values were published to the host.
    bool beginGesture(const Config& before, std::string_view label, EditKind kind = EditKind::Gesture);
    CommitResult endGesture(const Config& after);
    void cancelGesture() noexcept { gestureOpen_ = false; }
    bool gestureOpen() const noexcept { return gestureOpen_; }

    // Apply to `cfg` and move the step between stacks. Return the applied transaction (valid until the
    // next mutating call) or nullptr when nothing to do / a gesture is open.
    const Transaction* undo(Config& cfg);
    const Transaction* redo(Config& cfg);

    bool canUndo() const noexcept { return !undo_.empty(); }
    bool canRedo() const noexcept { return !redo_.empty(); }
    std::size_t undoSize() const noexcept { return undo_.size(); }
    std::size_t redoSize() const noexcept { return redo_.size(); }
    const Transaction* peekUndo() const noexcept { return undo_.empty() ? nullptr : &undo_.back(); }
    const Transaction* peekRedo() const noexcept { return redo_.empty() ? nullptr : &redo_.back(); }
    std::size_t bytes() const noexcept { return bytes_; }
    std::size_t budget() const noexcept { return budget_; }
    void clear() noexcept;  // project load

private:
    CommitResult record(Transaction t);
    std::deque<Transaction> undo_, redo_;  // back = most recent / next to apply
    std::size_t bytes_ = 0, budget_;
    bool gestureOpen_ = false;
    Config gestureBefore_;
    std::string gestureLabel_;
    EditKind gestureKind_ = EditKind::Gesture;
};

}  // namespace cd::state
