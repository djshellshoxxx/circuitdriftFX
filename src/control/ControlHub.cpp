// CircuitDrift control hub implementation. See ControlHub.h for the role/ownership contract.
#include "control/ControlHub.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>
#include "control/LatestSlot.h"
#include "control/SpscRing.h"

namespace cd::control {
namespace {
constexpr std::size_t kNoIndex = static_cast<std::size_t>(-1);
static_assert(kEventRingCapacity >= 2 && (kEventRingCapacity & (kEventRingCapacity - 1)) == 0);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

std::size_t paramIndex(std::uint32_t hostId) noexcept {
    const core::ParamSpec* p = core::findByHostId(hostId);
    return p == nullptr ? kNoIndex : static_cast<std::size_t>(p - core::kParams);
}
bool valueAllowed(const core::ParamSpec& p, double v) noexcept {
    if (!std::isfinite(v) || v < p.min || v > p.max) return false;
    return p.kind == core::ParamKind::Float || v == std::floor(v);
}
std::uint64_t packAck(std::uint64_t seq, RestoreStatus s) noexcept {
    return (seq << 8) | static_cast<std::uint64_t>(s);
}
}  // namespace

struct ControlHub::Impl {
    struct RestoreRequest {
        std::uint64_t seq = 0;
        CheckpointBlock cp;
    };
    struct Counters {
        std::atomic<std::uint64_t> inputRejected{0}, eventsDeferred{0}, eventsCoalesced{0}, eventsSuperseded{0},
            eventsStale{0}, configsStale{0}, restoresRejected{0}, capturesPublished{0}, capturesDiscarded{0};
    };
    static void bump(std::atomic<std::uint64_t>& c, std::uint64_t n = 1) noexcept {
        c.fetch_add(n, std::memory_order_relaxed);
    }

    // ---- shared atomics ----
    std::atomic<std::uint64_t> generation{1};
    std::atomic<std::uint64_t> layoutId{0};
    std::atomic<std::uint64_t> captureReq{0};   // worker -> audio
    std::atomic<std::uint64_t> captureDone{0};  // audio -> worker (set after the slot publish)
    std::atomic<std::uint64_t> restoreAck{0};   // audio -> worker: (seq << 8) | status
    Counters ctr;

    // ---- slots ----
    LatestSlot<ConfigBlock> configSlot;         // producer: UI (under uiMutex); consumer: audio
    SpscRing<ParamEvent, kEventRingCapacity> eventRing;  // producer: UI (under uiMutex); consumer: audio
    LatestSlot<RestoreRequest> restoreSlot;     // producer: worker (under workerMutex); consumer: audio
    LatestSlot<CheckpointBlock> checkpointSlot; // producer: audio; consumer: worker (under workerMutex)

    // ---- UI role state (uiMutex) ----
    mutable std::mutex uiMutex;
    ConfigBlock staged, committed;
    bool parkedFlag[kConfigParamCount] = {};
    double parkedValue[kConfigParamCount] = {};
    std::uint16_t parkedOrder[kConfigParamCount] = {};
    std::size_t parkedCount = 0;

    // ---- worker role state (workerMutex) ----
    std::mutex workerMutex;
    std::uint64_t captureSeq = 0, restoreSeq = 0;
    CheckpointBlock lastCp;
    bool lastCpValid = false;

    // ---- audio role state (audio thread only) ----
    struct AudioState {
        std::uint64_t gen = 0;
        ConfigBlock current;
        SampleClock clock;
        BlockControl block;
        ParamEvent events[kEventRingCapacity];
        std::uint64_t captureWanted = 0, captureServed = 0;
        bool restoreOpen = false;
        std::uint64_t restoreSeq = 0;
        const CheckpointBlock* restoreCp = nullptr;
        bool captureOpen = false;
    } audio;

    Impl() {
        for (std::size_t i = 0; i < kConfigParamCount; ++i) staged.values[i] = core::kParams[i].def;
        staged.generation = 1;
        staged.revision = 1;
        committed = staged;
        publishConfigLocked();
    }

    // ---- UI helpers (call with uiMutex held) ----
    void publishConfigLocked() noexcept {
        configSlot.writeBuffer() = committed;
        configSlot.publish();
    }
    ParamEvent makeEvent(std::size_t idx, double v) const noexcept {
        ParamEvent e;
        e.generation = committed.generation;
        e.revision = committed.revision;
        e.hostId = core::kParams[idx].hostId;
        e.value = v;
        return e;
    }
    void parkLocked(std::size_t idx, double v) noexcept {
        if (parkedFlag[idx]) {
            parkedValue[idx] = v;
            bump(ctr.eventsCoalesced);
        } else {
            parkedFlag[idx] = true;
            parkedValue[idx] = v;
            parkedOrder[parkedCount++] = static_cast<std::uint16_t>(idx);
            bump(ctr.eventsDeferred);
        }
    }
    std::size_t flushLocked() noexcept {
        std::size_t moved = 0;
        while (moved < parkedCount) {
            const std::size_t idx = parkedOrder[moved];
            if (!eventRing.tryPush(makeEvent(idx, parkedValue[idx]))) break;
            parkedFlag[idx] = false;
            ++moved;
        }
        if (moved != 0) {
            std::copy(parkedOrder + moved, parkedOrder + parkedCount, parkedOrder);
            parkedCount -= moved;
        }
        return parkedCount;
    }
    void supersedeParkedLocked() noexcept {
        for (std::size_t i = 0; i < parkedCount; ++i) parkedFlag[parkedOrder[i]] = false;
        bump(ctr.eventsSuperseded, parkedCount);
        parkedCount = 0;
    }

    // ---- worker helper (workerMutex held) ----
    void pollCheckpointLocked() noexcept {
        if (const CheckpointBlock* cp = checkpointSlot.acquire()) {
            lastCp = *cp;
            lastCpValid = true;
        }
    }
    void ack(std::uint64_t seq, RestoreStatus s) noexcept {
        restoreAck.store(packAck(seq, s), std::memory_order_release);
        if (s != RestoreStatus::Applied) bump(ctr.restoresRejected);
    }
};

ControlHub::ControlHub() : impl_(std::make_unique<Impl>()) {}
ControlHub::~ControlHub() = default;

// ======================= UI role =======================
bool ControlHub::stage(std::uint32_t hostId, double v) {
    const std::size_t idx = paramIndex(hostId);
    if (idx == kNoIndex || !valueAllowed(core::kParams[idx], v)) {
        Impl::bump(impl_->ctr.inputRejected);
        return false;
    }
    std::lock_guard<std::mutex> lk(impl_->uiMutex);
    impl_->staged.values[idx] = v;
    return true;
}

std::uint64_t ControlHub::commit() {
    std::lock_guard<std::mutex> lk(impl_->uiMutex);
    impl_->committed.values = impl_->staged.values;
    impl_->committed.revision += 1;
    impl_->staged.revision = impl_->committed.revision;
    impl_->publishConfigLocked();
    impl_->supersedeParkedLocked();
    return impl_->committed.revision;
}

bool ControlHub::sendParam(std::uint32_t hostId, double v) {
    const std::size_t idx = paramIndex(hostId);
    if (idx == kNoIndex || !valueAllowed(core::kParams[idx], v)) {
        Impl::bump(impl_->ctr.inputRejected);
        return false;
    }
    std::lock_guard<std::mutex> lk(impl_->uiMutex);
    impl_->staged.values[idx] = v;
    impl_->committed.values[idx] = v;
    if (impl_->parkedCount == 0) {
        if (impl_->eventRing.tryPush(impl_->makeEvent(idx, v))) return true;
        impl_->parkLocked(idx, v);
    } else {
        impl_->parkLocked(idx, v);
        impl_->flushLocked();
    }
    return true;
}

std::size_t ControlHub::flushEvents() {
    std::lock_guard<std::mutex> lk(impl_->uiMutex);
    return impl_->flushLocked();
}

std::uint64_t ControlHub::beginGeneration(std::uint64_t layoutId) {
    std::lock_guard<std::mutex> lk(impl_->uiMutex);
    const std::uint64_t g = impl_->generation.load(std::memory_order_relaxed) + 1;
    impl_->layoutId.store(layoutId, std::memory_order_relaxed);
    // Order matters: audio loads the generation AFTER acquiring slot items, so publishing the
    // generation first guarantees any new-generation item it sees implies the new generation.
    impl_->generation.store(g, std::memory_order_release);
    impl_->committed.generation = g;
    impl_->staged.generation = g;
    impl_->committed.revision += 1;
    impl_->staged.revision = impl_->committed.revision;
    impl_->publishConfigLocked();
    impl_->supersedeParkedLocked();
    return g;
}

std::uint64_t ControlHub::generation() const noexcept { return impl_->generation.load(std::memory_order_acquire); }

ConfigBlock ControlHub::committedConfig() const {
    std::lock_guard<std::mutex> lk(impl_->uiMutex);
    return impl_->committed;
}

// ======================= Worker role =======================
std::uint64_t ControlHub::requestRestore(const CheckpointBlock& cp, std::uint64_t expectedGeneration) {
    if (!cp.countsValid()) return 0;
    std::lock_guard<std::mutex> lk(impl_->workerMutex);
    const std::uint64_t seq = ++impl_->restoreSeq;
    auto& req = impl_->restoreSlot.writeBuffer();
    req.seq = seq;
    req.cp = cp;
    req.cp.generation = expectedGeneration;
    impl_->restoreSlot.publish();
    return seq;
}

RestoreStatus ControlHub::restoreStatus(std::uint64_t seq) const noexcept {
    const std::uint64_t w = impl_->restoreAck.load(std::memory_order_acquire);
    const std::uint64_t ackSeq = w >> 8;
    if (seq == 0 || ackSeq < seq) return RestoreStatus::Pending;
    if (ackSeq > seq) return RestoreStatus::Superseded;
    return static_cast<RestoreStatus>(w & 0xFFu);
}

void ControlHub::save(SaveResult& out, std::chrono::microseconds timeout) {
    std::lock_guard<std::mutex> lk(impl_->workerMutex);
    const std::uint64_t req = ++impl_->captureSeq;
    impl_->captureReq.store(req, std::memory_order_release);
    if (timeout.count() > 0) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (impl_->captureDone.load(std::memory_order_acquire) < req && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    const bool fresh = impl_->captureDone.load(std::memory_order_acquire) >= req;
    impl_->pollCheckpointLocked();  // sees every publication that preceded the observed captureDone
    out.config = committedConfig();
    out.hasCheckpoint = false;
    out.checkpointMatchesConfig = false;
    out.status = CheckpointStatus::Unavailable;
    if (impl_->lastCpValid && impl_->lastCp.generation == generation()) {
        out.hasCheckpoint = true;
        out.checkpoint = impl_->lastCp;
        out.status = fresh ? CheckpointStatus::Fresh : CheckpointStatus::StaleFallback;
        out.checkpointMatchesConfig = out.checkpoint.configRevision == out.config.revision;
    }
}

Diagnostics ControlHub::diagnostics() const noexcept {
    const auto& c = impl_->ctr;
    auto ld = [](const std::atomic<std::uint64_t>& a) { return a.load(std::memory_order_relaxed); };
    Diagnostics d;
    d.inputRejected = ld(c.inputRejected);
    d.eventsDeferred = ld(c.eventsDeferred);
    d.eventsCoalesced = ld(c.eventsCoalesced);
    d.eventsSuperseded = ld(c.eventsSuperseded);
    d.eventsStale = ld(c.eventsStale);
    d.configsStale = ld(c.configsStale);
    d.restoresRejected = ld(c.restoresRejected);
    d.capturesPublished = ld(c.capturesPublished);
    d.capturesDiscarded = ld(c.capturesDiscarded);
    return d;
}

// ======================= Audio role =======================
bool ControlHub::prepareClock(std::uint32_t sampleRateHz, std::uint32_t tickHz) noexcept {
    return impl_->audio.clock.prepare(sampleRateHz, tickHz);
}

const BlockControl& ControlHub::beginBlock(std::uint32_t frames) noexcept {
    auto& a = impl_->audio;
    if (a.restoreOpen) {  // engine skipped commitRestore for the previous block
        impl_->ack(a.restoreSeq, RestoreStatus::NotCommitted);
        a.restoreOpen = false;
    }
    a.block = BlockControl{};
    a.block.frames = frames;
    a.captureOpen = false;

    // 1. Acquire items (events first, then config, then restore), 2. load generation LAST.
    std::size_t n = 0;
    ParamEvent ev;
    while (n < kEventRingCapacity && impl_->eventRing.tryPop(ev)) a.events[n++] = ev;
    const ConfigBlock* cfg = impl_->configSlot.acquire();
    const Impl::RestoreRequest* rr = impl_->restoreSlot.acquire();
    const std::uint64_t g = impl_->generation.load(std::memory_order_acquire);

    if (g != a.gen) {
        a.gen = g;
        a.current = ConfigBlock{};
        a.current.generation = g;
        a.block.generationChanged = true;
    }
    if (cfg != nullptr) {
        if (cfg->generation == a.gen && cfg->revision > a.current.revision) {
            a.current = *cfg;
            a.block.configChanged = true;
        } else {
            Impl::bump(impl_->ctr.configsStale);
        }
    }
    std::size_t kept = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (a.events[i].generation == a.gen && a.events[i].revision >= a.current.revision)
            a.events[kept++] = a.events[i];
    }
    if (kept != n) Impl::bump(impl_->ctr.eventsStale, n - kept);
    a.block.eventCount = static_cast<std::uint32_t>(kept);
    a.block.events = a.events;

    if (rr != nullptr) {
        if (rr->cp.generation != a.gen) {
            impl_->ack(rr->seq, RestoreStatus::RejectedStaleGeneration);
        } else if (!a.clock.canRestore(rr->cp.sampleRateHz, rr->cp.clockRemainder) || !rr->cp.countsValid()) {
            impl_->ack(rr->seq, RestoreStatus::RejectedIncompatible);
        } else {
            a.block.restore = &rr->cp;
            a.restoreCp = &rr->cp;
            a.restoreSeq = rr->seq;
            a.restoreOpen = true;
        }
    }

    const std::uint64_t req = impl_->captureReq.load(std::memory_order_acquire);
    if (req > a.captureServed) {
        a.captureWanted = req;
        a.block.captureRequested = true;
    }
    return a.block;
}

const ConfigBlock& ControlHub::currentConfig() const noexcept { return impl_->audio.current; }
std::uint64_t ControlHub::audioGeneration() const noexcept { return impl_->audio.gen; }
const SampleClock& ControlHub::clock() const noexcept { return impl_->audio.clock; }

void ControlHub::commitRestore(bool applied) noexcept {
    auto& a = impl_->audio;
    if (!a.restoreOpen) return;
    a.restoreOpen = false;
    if (applied && a.clock.restore(a.restoreCp->sampleIndex, a.restoreCp->sampleRateHz, a.restoreCp->clockRemainder))
        impl_->ack(a.restoreSeq, RestoreStatus::Applied);
    else
        impl_->ack(a.restoreSeq, RestoreStatus::RejectedIncompatible);
}

CheckpointBlock* ControlHub::captureTarget() noexcept {
    auto& a = impl_->audio;
    if (!a.block.captureRequested) return nullptr;
    CheckpointBlock& cp = impl_->checkpointSlot.writeBuffer();
    cp.generation = a.gen;
    cp.layoutId = impl_->layoutId.load(std::memory_order_relaxed);
    cp.configRevision = a.current.revision;
    cp.sampleIndex = a.clock.sampleIndex();
    cp.clockRemainder = a.clock.remainder();
    cp.sampleRateHz = a.clock.sampleRateHz();
    cp.electricalCount = cp.thermalCount = cp.rngCount = 0;
    a.captureOpen = true;
    return &cp;
}

void ControlHub::publishCapture() noexcept {
    auto& a = impl_->audio;
    if (!a.captureOpen) return;
    a.captureOpen = false;
    a.block.captureRequested = false;
    a.captureServed = a.captureWanted;  // stop re-offering this request either way
    if (!impl_->checkpointSlot.writeBuffer().countsValid()) {
        Impl::bump(impl_->ctr.capturesDiscarded);
        return;
    }
    impl_->checkpointSlot.publish();
    impl_->captureDone.store(a.captureWanted, std::memory_order_release);
    Impl::bump(impl_->ctr.capturesPublished);
}

std::uint64_t ControlHub::endBlock() noexcept {
    auto& a = impl_->audio;
    if (a.restoreOpen) {
        impl_->ack(a.restoreSeq, RestoreStatus::NotCommitted);
        a.restoreOpen = false;
    }
    const std::uint32_t frames = a.block.frames;
    a.block.frames = 0;  // a repeated endBlock() cannot advance twice
    return a.clock.advance(frames);
}

}  // namespace cd::control
