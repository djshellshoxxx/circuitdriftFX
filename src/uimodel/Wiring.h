// CircuitDrift uimodel: UX-01 gesture/edit wiring to the engine's host-parameter path.
// Owner: cd_uimodel. Message thread only (allocates); it is the single producer of the
// cd::state::TransactionQueue it is given. Never touches audio state directly.
//  * EngineEditHost: EditHost implementation. View-model ids that are registry ids are
//    normalized with cd::core::toNormalized and pushed as Param TransactionEvents (host IDs).
//    View-model-only ids (tube.bias, supply.capacitance, age.<id>, material.<id>, ...) route to a
//    registered Mod Slot (slot.<a|b>.mod.NN, linear range map) or a blueprint-transaction sink,
//    else are REPORTED as unbound (counter + list + observer), never silently dropped.
//  * begin/perform/end semantics are tracked per id; protocol violations are counted.
//  * Publish-rate accounting (injected microsecond clock): peak performEdit pushes per id in any
//    1 s window, for the UX-01 "<= 120 Hz" check.
//  * UndoRedoController: History undo/redo restores the document and publishes ONLY the changed
//    values through the same EditHost, i.e. ordinary ParamEvents that take the normal smoothing
//    ramps; there is no physics rewind and no history entry is created.
//  * applyHostAutomation: host-origin edits update the document and History's current document
//    as TxOrigin::Automation (excluded from undo) and are never echoed back to the queue.
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "core/ParameterRegistry.h"
#include "state/History.h"
#include "state/TransactionQueue.h"
#include "uimodel/Document.h"
#include "uimodel/Gesture.h"

namespace cd::uimodel {

enum class Route : std::uint8_t { Registry, ModSlot, Blueprint, Unbound };

struct WiringStats {
    std::int64_t pushed = 0;          // Param events accepted by the queue (Queued or Coalesced)
    std::int64_t queueRejected = 0;   // queue refused (unknown host ID / non-finite / out of range)
    std::int64_t badValues = 0;       // non-finite plain values refused before the queue
    std::int64_t unbound = 0;         // edits on ids with no route
    std::int64_t blueprintEdits = 0, blueprintRefused = 0;
    std::int64_t protocolErrors = 0;  // perform/end without begin, nested begin
    std::int64_t gesturesEnded = 0;
};

class EngineEditHost : public EditHost {
public:
    using ClockUs = std::function<std::int64_t()>;
    using BlueprintSink = std::function<bool(const std::string& id, double plain)>;
    using UnboundObserver = std::function<void(const std::string& id)>;

    explicit EngineEditHost(state::TransactionQueue& queue, ClockUs clock = {});

    // Mod Slot route: plain [min,max] maps linearly onto the slot's normalized 0..1.
    // slot 1..16; scope SlotA or SlotB only. False when invalid or the id is a registry id.
    bool bindModSlot(const std::string& id, core::ParamScope scope, int slot, double min, double max);
    // Blueprint route (stored in the blueprint, never host IDs): the sink performs the transaction.
    bool bindBlueprint(const std::string& id, BlueprintSink sink);
    void setUnboundObserver(UnboundObserver o) { observer_ = std::move(o); }
    Route routeOf(const std::string& id) const;

    void beginEdit(const std::string& id) override;
    void performEdit(const std::string& id, double plainValue) override;
    void endEdit(const std::string& id) override;

    // Pushes the retained pending values (ring was full); call from the message-thread timer.
    bool flush() { return queue_.flushPending(); }

    const WiringStats& stats() const noexcept { return stats_; }
    const std::vector<std::string>& unboundIds() const noexcept { return unboundIds_; }
    bool editOpen(const std::string& id) const { return open_.count(id) != 0; }
    std::size_t openEdits() const noexcept { return open_.size(); }
    // Peak number of performEdit publishes for one id inside any 1 s window (0 without a clock).
    int peakPublishesPerSecond() const noexcept { return peakPerSecond_; }

private:
    struct ModBinding { std::uint32_t hostId; double min, max; };
    bool route(const std::string& id, double plain);
    void noteRate(const std::string& id);
    state::TransactionQueue& queue_;
    ClockUs clock_;
    std::map<std::string, ModBinding> mod_;
    std::map<std::string, BlueprintSink> bp_;
    std::map<std::string, bool> open_;
    std::map<std::string, std::deque<std::int64_t>> window_;
    std::vector<std::string> unboundIds_;
    UnboundObserver observer_;
    WiringStats stats_;
    int peakPerSecond_ = 0;
};

class UndoRedoController {
public:
    UndoRedoController(ParamDocument& doc, state::History& history, EditHost& host)
        : doc_(doc), hist_(history), host_(host) {}
    bool undo() { return apply(true); }
    bool redo() { return apply(false); }
    // Ids whose values were republished by the last undo/redo (document order).
    const std::vector<std::string>& lastPublished() const noexcept { return last_; }
private:
    bool apply(bool undo);
    ParamDocument& doc_;
    state::History& hist_;
    EditHost& host_;
    std::vector<std::string> last_;
};

// Host-origin edit of registry parameter `hostId` (normalized): updates the document and History's
// live document without creating an undo step. Returns false for unknown ids / bad values, or when
// the id is not in the document. Never pushes to a queue (the host already delivered the value).
bool applyHostAutomation(ParamDocument& doc, state::History& history, std::uint32_t hostId, double normalized);

}  // namespace cd::uimodel
