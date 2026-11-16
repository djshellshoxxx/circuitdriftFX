// CircuitDrift state: SnapshotExchange implementation (triple buffer + reader-only mutex).
// Owner: cd_state. Audio-side functions are noexcept and allocation/lock free; readers and
// hostSave run on non-audio threads. See SnapshotExchange.h.
#include "state/SnapshotExchange.h"
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace cd::state {

SnapshotExchange::SnapshotExchange(const SnapshotIdentity& identity) : identity_(identity) {
    const std::uint32_t n = identity.layout.byteSize;
    if (n == 0 || n > 64u * 1024u * 1024u) throw std::invalid_argument("snapshot payload size out of range");
    if (!(std::isfinite(identity.sampleRate) && identity.sampleRate > 0) || identity.oversampling == 0)
        throw std::invalid_argument("snapshot identity invalid");
    for (Slot& s : slots_) {
        s.payload = std::make_unique<std::byte[]>(n);  // value-initialised (zero)
        s.header.identity = identity;
    }
}

std::uint64_t SnapshotExchange::requestCapture() noexcept {
    return requested_.fetch_add(1, std::memory_order_acq_rel) + 1;
}

bool SnapshotExchange::captureRequested() const noexcept {
    return requested_.load(std::memory_order_acquire) > served_;
}

std::span<std::byte> SnapshotExchange::writePayload() noexcept {
    serving_ = requested_.load(std::memory_order_acquire);
    return {slots_[back_].payload.get(), identity_.layout.byteSize};
}

void SnapshotExchange::publish(std::uint64_t sampleIndex, double clockRemainder, std::uint64_t configRevision) noexcept {
    SnapshotHeader& h = slots_[back_].header;
    h.identity = identity_;
    h.sampleIndex = sampleIndex;
    h.clockRemainder = clockRemainder;
    h.configRevision = configRevision;
    h.publishSequence = ++sequence_;
    if (serving_ > served_) served_ = serving_;
    h.servedRequest = served_;
    const std::uint8_t prev = middle_.exchange(std::uint8_t(back_ | kFresh), std::memory_order_acq_rel);
    back_ = std::uint8_t(prev & 0x3);
    published_.store(sequence_, std::memory_order_release);
}

void SnapshotExchange::readLatest(SavedSnapshot& out) const {
    std::lock_guard<std::mutex> lock(readerMutex_);
    if (middle_.load(std::memory_order_relaxed) & kFresh) {
        const std::uint8_t prev = middle_.exchange(front_, std::memory_order_acq_rel);
        front_ = std::uint8_t(prev & 0x3);
        hasFront_ = true;
    }
    out.valid = hasFront_;
    if (!hasFront_) { out.header = SnapshotHeader{}; out.payload.clear(); return; }
    const Slot& s = slots_[front_];
    out.header = s.header;
    out.payload.assign(s.payload.get(), s.payload.get() + identity_.layout.byteSize);
}

HostSaveResult hostSave(const SnapshotExchange& exchange, std::string configuration) {
    HostSaveResult r;
    r.configuration = std::move(configuration);
    exchange.readLatest(r.snapshot);
    if (r.snapshot.valid) {
        r.decision = RestoreDecision::Restore;
        r.reason = "last coherent published snapshot at sample " + std::to_string(r.snapshot.header.sampleIndex);
    } else {
        r.decision = RestoreDecision::NoCheckpoint;
        r.reason = "no checkpoint; configuration only (no exact continuity claimed)";
    }
    return r;
}

RestoreDecision decideSnapshotRestore(const SavedSnapshot& saved, const SnapshotIdentity& rt, std::string& reason) {
    if (!saved.valid) { reason = "no checkpoint; configuration only (no exact continuity claimed)"; return RestoreDecision::NoCheckpoint; }
    const SnapshotIdentity& s = saved.header.identity;
    if (s.layout.layoutId != rt.layout.layoutId) reason = "provider layout id differs";
    else if (s.layout.version != rt.layout.version) reason = "provider layout version differs";
    else if (s.layout.byteSize != rt.layout.byteSize || saved.payload.size() != rt.layout.byteSize) reason = "provider layout size differs";
    else if (s.modelId != rt.modelId) reason = "engine model identity differs";
    else if (s.sampleRate != rt.sampleRate) reason = "sample rate differs";
    else if (s.oversampling != rt.oversampling) reason = "oversampling differs";
    else { reason.clear(); return RestoreDecision::Restore; }
    reason += "; dynamic state reset to prepared warm initialisation (configuration only)";
    return RestoreDecision::ResetWarm;
}

RestoreDecision restoreSnapshot(const SavedSnapshot& saved, const SnapshotIdentity& rt,
                                std::span<std::byte> liveState, std::string& reason) {
    const RestoreDecision d = decideSnapshotRestore(saved, rt, reason);
    if (d != RestoreDecision::Restore) return d;
    if (liveState.size() != saved.payload.size()) {
        reason = "live state buffer size differs; dynamic state reset to prepared warm initialisation (configuration only)";
        return RestoreDecision::ResetWarm;
    }
    std::memcpy(liveState.data(), saved.payload.data(), saved.payload.size());
    return RestoreDecision::Restore;
}

}  // namespace cd::state
