// CircuitDrift state: twenty-step configuration undo/redo (CD-F09 / T09, 05-state "Twenty-step
// undo/redo"). Owner: cd_state.
// Each transaction holds {label, origin, affected IDs, previous, next} configuration documents.
// Documents are immutable shared blobs (canonical JSON text, e.g. saveState() output), so a
// state shared by consecutive transactions is stored once. A new edit clears redo; automation
// and other external origins are never recorded (automation updates the current document
// only). Wheel commits on the same IDs separated by < 250 ms idle time are grouped into one
// transaction (clock injected). Project load replaces the state and clears both stacks.
// Budget: at most kSteps (20) transactions are retained; every blob plus metadata is accounted
// (string capacity + fixed overheads) and the hard cap is 56 MiB. Blobs larger than the 2.5 MiB
// session bound are rejected, so 20 steps (21 distinct worst-case sessions) always fit; if a
// smaller configured budget is exceeded, the oldest transactions are evicted first.
// Undo changes configuration only; it never rewinds the physics clock or stores checkpoints.
// Threading: message thread only (allocates). Publishing a restored document to audio goes
// through the normal transaction path (TransactionQueue/GenerationPool), not from here.
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "state/StateDocument.h"

namespace cd::state {

using ConfigBlob = std::shared_ptr<const std::string>;

enum class TxOrigin : std::uint8_t {
    Gesture,      // completed drag, numeric edit, Apply graph/fault, timeline scrub, macro change
    Wheel,        // grouped by 250 ms idle time
    SceneLoad,    // scene load into the selected slot (one visible transaction)
    AbOperation,  // A/B copy or swap
    Automation,   // host-driven: excluded
    External,     // meters, transport, cloud refresh, publish/save/account: excluded
};

enum class CommitResult : std::uint8_t { Recorded, Grouped, Excluded, NoChange, Rejected };

struct HistoryTransaction {
    std::string label;
    TxOrigin origin = TxOrigin::Gesture;
    std::vector<std::string> affectedIds;
    ConfigBlob prev, next;
    std::int64_t lastEditMs = 0;
};

class History {
public:
    using Clock = std::function<std::int64_t()>;  // monotonic milliseconds
    static constexpr std::size_t kSteps = 20;
    static constexpr std::size_t kBudgetBytes = std::size_t(56) * 1024 * 1024;
    static constexpr std::int64_t kWheelIdleMs = 250;
    static constexpr std::size_t kMaxBlobBytes = kSessionMaxBytes;
    static constexpr std::size_t kMaxLabelBytes = 256, kMaxIds = 64, kMaxIdBytes = 128;
    static constexpr std::size_t kBlobOverhead = 64;  // shared_ptr control block + string object

    explicit History(Clock clock = {}, std::size_t budgetBytes = kBudgetBytes);

    // Creates an immutable blob with capacity trimmed to its size (honest accounting).
    static ConfigBlob makeBlob(std::string text);
    static ConfigBlob makeBlob(const StateDocument& doc) { return makeBlob(saveState(doc)); }

    // Project load / initial state: replaces the current document and clears undo and redo.
    void load(ConfigBlob state);
    CommitResult commit(std::string label, TxOrigin origin, std::vector<std::string> affectedIds,
                        ConfigBlob next, std::string* error = nullptr);
    // Returns the new current document, or nullptr when there is nothing to undo/redo.
    ConfigBlob undo();
    ConfigBlob redo();

    const ConfigBlob& current() const noexcept { return current_; }
    std::size_t undoDepth() const noexcept { return undo_.size(); }
    std::size_t redoDepth() const noexcept { return redo_.size(); }
    const HistoryTransaction* nextUndo() const noexcept { return undo_.empty() ? nullptr : &undo_.back(); }
    const HistoryTransaction* nextRedo() const noexcept { return redo_.empty() ? nullptr : &redo_.back(); }
    std::size_t accountedBytes() const;
    std::size_t budgetBytes() const noexcept { return budget_; }
    std::size_t evictedForBudget() const noexcept { return evictedForBudget_; }

private:
    std::int64_t now() const;
    static std::size_t metadataBytes(const HistoryTransaction& t) noexcept;
    Clock clock_;
    std::size_t budget_;
    ConfigBlob current_;
    std::deque<HistoryTransaction> undo_;   // back = most recent
    std::vector<HistoryTransaction> redo_;  // back = next redo
    bool wheelGroupOpen_ = false;
    std::size_t evictedForBudget_ = 0;
};

}  // namespace cd::state
