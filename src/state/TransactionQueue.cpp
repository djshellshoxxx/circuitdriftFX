// CircuitDrift state: TransactionQueue implementation. See TransactionQueue.h for the
// ownership and overflow contract. Owner: cd_state. push() runs on the single producer thread,
// drainSlice() on the audio thread; both are allocation/lock/I-O free and bounded.
#include "state/TransactionQueue.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace cd::state {
namespace {
static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "atomic<uint64_t> must be lock-free (UX-09)");
static_assert(kTimedCapacity > 0 && kTimedCapacity + kNoteReserve + kCoalesceReserve == kSliceEventCapacity);

bool validEvent(const TransactionEvent& e) noexcept {
    switch (e.kind) {
        case EventKind::Param:
            return std::isfinite(e.normalized) && e.normalized >= 0.0 && e.normalized <= 1.0;
        case EventKind::ConfigPublish:
            return e.generation != 0 && e.generation <= kMaxGeneration && e.handleIndex < 256;
        case EventKind::NoteOff:
            return e.channel < 16 && e.note < 128;
        case EventKind::AllNotesOff:
            return e.channel < 16 || e.channel == kOmniChannel;
    }
    return false;
}

// Stable bottom-up merge sort by sampleOffset using a preallocated scratch buffer.
void stableSortByOffset(TransactionEvent* a, TransactionEvent* tmp, std::size_t n) noexcept {
    for (std::size_t width = 1; width < n; width *= 2) {
        for (std::size_t lo = 0; lo < n; lo += 2 * width) {
            const std::size_t mid = std::min(lo + width, n), hi = std::min(lo + 2 * width, n);
            std::size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) tmp[k++] = (a[j].sampleOffset < a[i].sampleOffset) ? a[j++] : a[i++];
            while (i < mid) tmp[k++] = a[i++];
            while (j < hi) tmp[k++] = a[j++];
        }
        std::copy(tmp, tmp + n, a);
    }
}
}  // namespace

TransactionQueue::TransactionQueue(std::size_t ringCapacity)
    : ring_(ringCapacity > 2 * kRingNoteReserve ? ringCapacity : throw std::invalid_argument("ring capacity too small")),
      pending_(std::make_unique<Pending[]>(core::kParamCount)),
      events_(std::make_unique<TransactionEvent[]>(kSliceEventCapacity)),
      scratch_(std::make_unique<TransactionEvent[]>(kSliceEventCapacity)),
      table_(std::make_unique<Coalesced[]>(core::kParamCount)) {}

bool TransactionQueue::tryRing(const TransactionEvent& e) noexcept {
    const bool note = e.kind == EventKind::NoteOff || e.kind == EventKind::AllNotesOff;
    return ring_.tryPush(e, note ? 0 : kRingNoteReserve);
}

bool TransactionQueue::flushPending() noexcept {
    if (pendingNotes_) {
        TransactionEvent off = TransactionEvent::allNotesOff(kOmniChannel, 0);
        off.coalesced = true;
        if (!tryRing(off)) return false;
        pendingNotes_ = false;
        flushed_.fetch_add(1, std::memory_order_relaxed);
    }
    if (pendingConfig_) {
        if (!tryRing(pendingConfigEvent_)) return false;
        pendingConfig_ = false;
        flushed_.fetch_add(1, std::memory_order_relaxed);
    }
    for (std::size_t i = 0; i < core::kParamCount && pendingCount_ != 0; ++i) {
        Pending& p = pending_[i];
        if (!p.dirty) continue;
        if (!tryRing(p.event)) return false;
        p.dirty = false;
        --pendingCount_;
        flushed_.fetch_add(1, std::memory_order_relaxed);
    }
    return pendingCount_ == 0;
}

PushResult TransactionQueue::push(const TransactionEvent& in) noexcept {
    if (!validEvent(in)) { rejected_.fetch_add(1, std::memory_order_relaxed); return PushResult::Rejected; }
    TransactionEvent e = in;
    e.registryIndex = 0;
    e.coalesced = false;
    if (e.kind == EventKind::Param) {
        const core::ParamSpec* spec = core::findByHostId(e.hostId);
        if (!spec) { rejected_.fetch_add(1, std::memory_order_relaxed); return PushResult::Rejected; }
        e.registryIndex = std::uint32_t(spec - core::kParams);
    }
    (void)flushPending();
    switch (e.kind) {
        case EventKind::Param: {
            Pending& p = pending_[e.registryIndex];
            // While a value for this parameter is pending, newer values replace it (latest wins).
            if (!p.dirty && tryRing(e)) return PushResult::Queued;
            if (!p.dirty) ++pendingCount_;
            p.dirty = true;
            p.event = e;
            p.event.coalesced = true;
            break;
        }
        case EventKind::ConfigPublish:
            if (!pendingConfig_ && tryRing(e)) return PushResult::Queued;
            if (!pendingConfig_ || e.generation > pendingConfigEvent_.generation) { pendingConfigEvent_ = e; pendingConfigEvent_.coalesced = true; }
            pendingConfig_ = true;
            break;
        case EventKind::NoteOff:
        case EventKind::AllNotesOff:
            if (!pendingNotes_ && tryRing(e)) return PushResult::Queued;
            if (!pendingNotes_) forced_.fetch_add(1, std::memory_order_relaxed);
            pendingNotes_ = true;  // upgraded to AllNotesOff(omni): release is guaranteed
            break;
    }
    ringCoalesced_.fetch_add(1, std::memory_order_relaxed);
    return PushResult::Coalesced;
}

void TransactionQueue::resetSlice() noexcept {
    count_ = timed_ = 0;
    for (std::size_t i = 0; i < core::kParamCount; ++i) table_[i] = Coalesced{};
    configPending_ = false;
    configLatest_ = TransactionEvent{};
    maxTimedConfigOffset_ = 0;
    forceAllNotesOff_ = false;
}

void TransactionQueue::coalesceParam(std::uint32_t index, double value, std::uint32_t offset) noexcept {
    Coalesced& c = table_[index];
    c.dirty = true;
    c.value = value;
    c.offset = std::max(c.offset, offset);
}

void TransactionQueue::coalesceConfig(const TransactionEvent& e) noexcept {
    if (!configPending_ || e.generation > configLatest_.generation) {
        const std::uint32_t off = std::max(e.sampleOffset, configPending_ ? configLatest_.sampleOffset : 0u);
        configLatest_ = e;
        configLatest_.sampleOffset = off;
    }
    configPending_ = true;
}

void TransactionQueue::place(const TransactionEvent& e) noexcept {
    constexpr std::size_t kAppendCapacity = kSliceEventCapacity - kCoalesceReserve;
    switch (e.kind) {
        case EventKind::Param: {
            if (e.registryIndex >= core::kParamCount) { rejected_.fetch_add(1, std::memory_order_relaxed); return; }
            Coalesced& c = table_[e.registryIndex];
            if (c.dirty || timed_ >= kTimedCapacity) {
                sliceCoalesced_.fetch_add(1, std::memory_order_relaxed);
                coalesceParam(e.registryIndex, e.normalized, e.sampleOffset);
                return;
            }
            events_[count_++] = e;
            ++timed_;
            c.maxTimedOffset = std::max(c.maxTimedOffset, e.sampleOffset);
            return;
        }
        case EventKind::ConfigPublish:
            if (configPending_ || timed_ >= kTimedCapacity) {
                sliceCoalesced_.fetch_add(1, std::memory_order_relaxed);
                coalesceConfig(e);
                return;
            }
            events_[count_++] = e;
            ++timed_;
            maxTimedConfigOffset_ = std::max(maxTimedConfigOffset_, e.sampleOffset);
            return;
        case EventKind::NoteOff:
        case EventKind::AllNotesOff:
            if (count_ >= kAppendCapacity) {
                if (!forceAllNotesOff_) forced_.fetch_add(1, std::memory_order_relaxed);
                forceAllNotesOff_ = true;
                return;
            }
            events_[count_++] = e;
            return;
    }
}

void TransactionQueue::emitCoalesced() noexcept {
    for (std::uint32_t i = 0; i < core::kParamCount; ++i) {
        const Coalesced& c = table_[i];
        if (!c.dirty) continue;
        TransactionEvent e = TransactionEvent::param(core::kParams[i].hostId, c.value, std::max(c.offset, c.maxTimedOffset));
        e.registryIndex = i;
        e.coalesced = true;
        events_[count_++] = e;
    }
    if (configPending_) {
        TransactionEvent e = configLatest_;
        e.sampleOffset = std::max(e.sampleOffset, maxTimedConfigOffset_);
        e.coalesced = true;
        events_[count_++] = e;
    }
    if (forceAllNotesOff_) {
        TransactionEvent e = TransactionEvent::allNotesOff(kOmniChannel, std::numeric_limits<std::uint32_t>::max());
        e.coalesced = true;
        events_[count_++] = e;
    }
}

void TransactionQueue::clampAndSort(std::uint32_t frames) noexcept {
    const std::uint32_t last = frames == 0 ? 0 : frames - 1;
    bool sorted = true;
    for (std::size_t i = 0; i < count_; ++i) {
        events_[i].sampleOffset = std::min(events_[i].sampleOffset, last);
        if (i && events_[i].sampleOffset < events_[i - 1].sampleOffset) sorted = false;
    }
    if (!sorted) stableSortByOffset(events_.get(), scratch_.get(), count_);
}

std::span<const TransactionEvent> TransactionQueue::drainSlice(std::uint32_t frames) noexcept {
    resetSlice();
    TransactionEvent e;
    for (std::size_t i = 0; i < ring_.capacity() && ring_.tryPop(e); ++i) place(e);
    emitCoalesced();
    clampAndSort(frames);
    return {events_.get(), count_};
}

QueueDiagnostics TransactionQueue::diagnostics() const noexcept {
    QueueDiagnostics d;
    d.ringCoalesced = ringCoalesced_.load(std::memory_order_relaxed);
    d.sliceCoalesced = sliceCoalesced_.load(std::memory_order_relaxed);
    d.forcedAllNotesOff = forced_.load(std::memory_order_relaxed);
    d.rejected = rejected_.load(std::memory_order_relaxed);
    d.flushed = flushed_.load(std::memory_order_relaxed);
    return d;
}

}  // namespace cd::state
