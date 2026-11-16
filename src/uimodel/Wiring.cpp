// CircuitDrift uimodel: engine edit wiring. See Wiring.h. Message thread only.
#include "uimodel/Wiring.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace cd::uimodel {

namespace {
constexpr std::int64_t kWindowUs = 1000000;
}

EngineEditHost::EngineEditHost(state::TransactionQueue& queue, ClockUs clock) : queue_(queue), clock_(std::move(clock)) {}

bool EngineEditHost::bindModSlot(const std::string& id, core::ParamScope scope, int slot, double min, double max) {
    if (id.empty() || core::findById(id.c_str()) || slot < 1 || slot > 16 || !std::isfinite(min) || !std::isfinite(max) || !(max > min))
        return false;
    if (scope != core::ParamScope::SlotA && scope != core::ParamScope::SlotB) return false;
    char name[32];
    std::snprintf(name, sizeof name, "slot.%c.mod.%02d", scope == core::ParamScope::SlotA ? 'a' : 'b', slot);
    const core::ParamSpec* s = core::findById(name);
    if (!s) return false;
    bp_.erase(id);
    mod_[id] = {s->hostId, min, max};
    return true;
}

bool EngineEditHost::bindBlueprint(const std::string& id, BlueprintSink sink) {
    if (id.empty() || !sink || core::findById(id.c_str())) return false;
    mod_.erase(id);
    bp_[id] = std::move(sink);
    return true;
}

Route EngineEditHost::routeOf(const std::string& id) const {
    if (core::findById(id.c_str())) return Route::Registry;
    if (mod_.count(id)) return Route::ModSlot;
    if (bp_.count(id)) return Route::Blueprint;
    return Route::Unbound;
}

void EngineEditHost::beginEdit(const std::string& id) {
    if (!open_.emplace(id, true).second) ++stats_.protocolErrors;
}

void EngineEditHost::endEdit(const std::string& id) {
    if (open_.erase(id) == 0) { ++stats_.protocolErrors; return; }
    ++stats_.gesturesEnded;
}

void EngineEditHost::noteRate(const std::string& id) {
    if (!clock_) return;
    const std::int64_t now = clock_();
    auto& w = window_[id];
    w.push_back(now);
    while (!w.empty() && now - w.front() >= kWindowUs) w.pop_front();
    peakPerSecond_ = std::max(peakPerSecond_, int(w.size()));
}

bool EngineEditHost::route(const std::string& id, double plain) {
    if (const core::ParamSpec* s = core::findById(id.c_str())) {
        const auto r = queue_.push(state::TransactionEvent::param(s->hostId, core::toNormalized(*s, plain), 0));
        if (r == state::PushResult::Rejected) { ++stats_.queueRejected; return false; }
        ++stats_.pushed;
        return true;
    }
    if (auto it = mod_.find(id); it != mod_.end()) {
        const ModBinding& b = it->second;
        const double u = std::clamp((plain - b.min) / (b.max - b.min), 0.0, 1.0);
        const auto r = queue_.push(state::TransactionEvent::param(b.hostId, u, 0));
        if (r == state::PushResult::Rejected) { ++stats_.queueRejected; return false; }
        ++stats_.pushed;
        return true;
    }
    if (auto it = bp_.find(id); it != bp_.end()) {
        if (it->second(id, plain)) { ++stats_.blueprintEdits; return true; }
        ++stats_.blueprintRefused;
        return false;
    }
    ++stats_.unbound;
    if (std::find(unboundIds_.begin(), unboundIds_.end(), id) == unboundIds_.end()) unboundIds_.push_back(id);
    if (observer_) observer_(id);
    return false;
}

void EngineEditHost::performEdit(const std::string& id, double plain) {
    if (!open_.count(id)) ++stats_.protocolErrors;   // still delivered: the host decides, never silently dropped
    if (!std::isfinite(plain)) { ++stats_.badValues; return; }
    if (route(id, plain)) noteRate(id);
}

bool UndoRedoController::apply(bool isUndo) {
    last_.clear();
    std::map<std::string, double> before;
    for (const auto& kv : doc_.entries()) before[kv.first] = kv.second.value;
    const state::ConfigBlob blob = isUndo ? hist_.undo() : hist_.redo();
    if (!blob) return false;
    if (!doc_.restore(*blob)) {
        // Unreadable document: put History back where it was; the live document is unchanged.
        if (isUndo) (void)hist_.redo(); else (void)hist_.undo();
        return false;
    }
    for (const auto& kv : doc_.entries()) {
        const auto it = before.find(kv.first);
        if (it != before.end() && it->second == kv.second.value) continue;
        last_.push_back(kv.first);
    }
    for (const auto& id : last_) {   // normal ParamEvents: the engine smooths them like any edit
        host_.beginEdit(id);
        host_.performEdit(id, doc_.get(id));
        host_.endEdit(id);
    }
    return true;
}

bool applyHostAutomation(ParamDocument& doc, state::History& history, std::uint32_t hostId, double normalized) {
    const core::ParamSpec* s = core::findByHostId(hostId);
    if (!s || !std::isfinite(normalized) || normalized < 0.0 || normalized > 1.0 || !doc.find(s->id)) return false;
    if (!doc.set(s->id, core::toPlain(*s, normalized))) return false;
    return history.commit("Host automation", state::TxOrigin::Automation, {s->id}, doc.blob()) == state::CommitResult::Excluded;
}

}  // namespace cd::uimodel
