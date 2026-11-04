// CircuitDrift state: message->audio parameter/configuration transactions and generation-
// tracked configuration slots (P04 queue interface; 01-runtime "event storage", UX-09).
// Owner: cd_state.
// Ownership/threading:
//  * TransactionQueue: exactly one producer thread (message/host-event thread) calls push();
//    exactly one consumer (the audio thread) calls drainSlice(). Construction allocates every
//    buffer; push() and drainSlice() are noexcept, lock-free and allocation-free. diagnostics()
//    may be read from any thread.
//  * GenerationPool<T,N>: one worker thread owns acquireFree/publish/reclaim (non-audio; it may
//    write slot payloads); the audio thread owns tryInstall/installLatest. Reclamation of a
//    slot happens only on the worker after the audio thread acknowledged a newer generation,
//    so no destruction/deallocation ever happens on audio.
// Overflow policy (never blocks, never reorders one parameter's values):
//  * Ring full (audio stopped or starved): the PRODUCER keeps the latest value per parameter,
//    the newest ConfigPublish and a sticky AllNotesOff in producer-private fixed tables and
//    retries them, in that priority (notes, config, params), at the start of every push() and
//    in flushPending() (call from the message-thread timer). Values for a parameter that is
//    pending replace the pending value, so per-parameter order is preserved (latest wins).
//    The last kRingNoteReserve ring slots are usable only by NoteOff/AllNotesOff.
//  * Slice storage holds kSliceEventCapacity (4096) events. Parameter/config events beyond
//    the timed region are coalesced by the CONSUMER to the latest value per registry index
//    (sample accuracy lost for that parameter in that slice; final value exact).
//  * NoteOff/AllNotesOff have kNoteReserve reserved slice slots; if those are exhausted the
//    slice is guaranteed to end with an AllNotesOff on kOmniChannel, so no note hangs.
//  * Every coalescing/forced/rejected case increments a diagnostic counter.
//  * Cross-parameter arrival order is not preserved under overflow; drained events are stably
//    ordered by sampleOffset, so offsets for one parameter within a slice must not decrease.
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include "core/ParameterRegistry.h"
#include "state/RtQueue.h"

namespace cd::state {

inline constexpr std::size_t kSliceEventCapacity = 4096;
inline constexpr std::size_t kNoteReserve = 256;                       // only note-off class may use these
inline constexpr std::size_t kCoalesceReserve = core::kParamCount + 2; // per-param + config + forced all-notes-off
inline constexpr std::size_t kTimedCapacity = kSliceEventCapacity - kNoteReserve - kCoalesceReserve;
inline constexpr std::size_t kDefaultRingCapacity = 8192;
inline constexpr std::size_t kRingNoteReserve = 64;  // ring slots only note-off class may fill
inline constexpr std::uint8_t kOmniChannel = 0xFF;  // AllNotesOff on every channel
inline constexpr std::uint64_t kMaxGeneration = (std::uint64_t(1) << 56) - 1;

enum class EventKind : std::uint8_t { Param, ConfigPublish, NoteOff, AllNotesOff };

// POD event; fields unused by a kind are zero.
struct TransactionEvent {
    EventKind kind = EventKind::Param;
    std::uint8_t channel = 0;      // NoteOff/AllNotesOff
    std::uint8_t note = 0;         // NoteOff
    std::uint32_t sampleOffset = 0;// within the consuming slice; clamped to frames-1 (0 for zero-frame)
    std::uint32_t hostId = 0;      // Param: permanent host ID (1000-1999)
    std::uint32_t handleIndex = 0; // ConfigPublish: GenerationPool slot
    double normalized = 0;         // Param: [0,1]
    std::uint64_t generation = 0;  // ConfigPublish: >0, strictly increasing per publisher
    std::uint32_t registryIndex = 0; // filled by the queue (index into core::kParams)
    bool coalesced = false;          // set on drained events that replaced earlier values (timing approximate)

    static TransactionEvent param(std::uint32_t hostId, double normalized, std::uint32_t offset) noexcept {
        TransactionEvent e; e.kind = EventKind::Param; e.hostId = hostId; e.normalized = normalized; e.sampleOffset = offset; return e;
    }
    static TransactionEvent config(std::uint64_t generation, std::uint32_t handle, std::uint32_t offset) noexcept {
        TransactionEvent e; e.kind = EventKind::ConfigPublish; e.generation = generation; e.handleIndex = handle; e.sampleOffset = offset; return e;
    }
    static TransactionEvent noteOff(std::uint8_t channel, std::uint8_t note, std::uint32_t offset) noexcept {
        TransactionEvent e; e.kind = EventKind::NoteOff; e.channel = channel; e.note = note; e.sampleOffset = offset; return e;
    }
    static TransactionEvent allNotesOff(std::uint8_t channel, std::uint32_t offset) noexcept {
        TransactionEvent e; e.kind = EventKind::AllNotesOff; e.channel = channel; e.sampleOffset = offset; return e;
    }
};

enum class PushResult : std::uint8_t { Queued, Coalesced, Rejected };

struct QueueDiagnostics {
    std::uint64_t ringCoalesced = 0;     // producer: ring full -> held/replaced in pending table
    std::uint64_t sliceCoalesced = 0;    // consumer: timed region full
    std::uint64_t forcedAllNotesOff = 0; // slice note reserve exhausted, or note-off deferred by full ring
    std::uint64_t rejected = 0;          // unknown hostId, non-finite/out-of-range value, bad generation
    std::uint64_t flushed = 0;           // producer: pending items later moved into the ring
};

class TransactionQueue {
public:
    explicit TransactionQueue(std::size_t ringCapacity = kDefaultRingCapacity);  // allocates
    TransactionQueue(const TransactionQueue&) = delete;
    TransactionQueue& operator=(const TransactionQueue&) = delete;

    // Producer thread only. Validates (unknown hostId, non-finite or out-of-[0,1] value,
    // generation 0/>kMaxGeneration, handle >= 256, bad channel/note -> Rejected); never blocks.
    // Queued: in the ring. Coalesced: held as latest value in the producer pending table.
    PushResult push(const TransactionEvent& e) noexcept;
    // Producer thread only. Retries pending (coalesced) items; returns true when none remain.
    bool flushPending() noexcept;
    bool hasPending() const noexcept { return pendingCount_ != 0 || pendingNotes_ || pendingConfig_; }

    // Audio thread only. Consumes what is in the ring (at most ring capacity) and returns the
    // slice's events stably sorted by sampleOffset (offsets clamped to frames-1, or 0 for a
    // zero-frame slice). The span stays valid until the next drainSlice call.
    std::span<const TransactionEvent> drainSlice(std::uint32_t frames) noexcept;

    QueueDiagnostics diagnostics() const noexcept;
    std::size_t ringCapacity() const noexcept { return ring_.capacity(); }

private:
    struct Pending {  // producer-private
        bool dirty = false;
        TransactionEvent event{};
    };
    struct Coalesced {  // consumer-private, per registry index
        bool dirty = false;
        double value = 0;
        std::uint32_t offset = 0;
        std::uint32_t maxTimedOffset = 0;
    };
    bool tryRing(const TransactionEvent& e) noexcept;
    void resetSlice() noexcept;
    void place(const TransactionEvent& e) noexcept;
    void coalesceParam(std::uint32_t index, double value, std::uint32_t offset) noexcept;
    void coalesceConfig(const TransactionEvent& e) noexcept;
    void emitCoalesced() noexcept;
    void clampAndSort(std::uint32_t frames) noexcept;

    SpscQueue<TransactionEvent> ring_;
    // producer-private
    std::unique_ptr<Pending[]> pending_;              // core::kParamCount entries
    std::size_t pendingCount_ = 0;
    bool pendingNotes_ = false;
    bool pendingConfig_ = false;
    TransactionEvent pendingConfigEvent_{};
    // consumer-private
    std::unique_ptr<TransactionEvent[]> events_;      // kSliceEventCapacity
    std::unique_ptr<TransactionEvent[]> scratch_;     // merge-sort buffer
    std::unique_ptr<Coalesced[]> table_;              // core::kParamCount
    std::size_t count_ = 0, timed_ = 0;
    bool configPending_ = false;
    TransactionEvent configLatest_{};
    std::uint32_t maxTimedConfigOffset_ = 0;
    bool forceAllNotesOff_ = false;
    // diagnostics (any thread reads)
    std::atomic<std::uint64_t> ringCoalesced_{0}, sliceCoalesced_{0}, forced_{0}, rejected_{0}, flushed_{0};
};

// Fixed-slot, generation-tracked configuration storage. T must be default-constructible; the
// worker writes payloads in place (no audio-side construction/destruction).
template <class T, std::size_t N>
class GenerationPool {
    static_assert(N >= 2 && N <= 256, "GenerationPool needs 2..256 slots");

public:
    static constexpr std::uint32_t kNone = 0xFFFFFFFFu;
    GenerationPool() : slots_(std::make_unique<T[]>(N)) {}
    GenerationPool(const GenerationPool&) = delete;
    GenerationPool& operator=(const GenerationPool&) = delete;

    // ---- worker thread ----
    // Reserves a free slot for writing; returns kNone when every slot is in use (call reclaim()).
    std::uint32_t acquireFree() noexcept {
        for (std::uint32_t i = 0; i < N; ++i)
            if (state_[i] == SlotState::Free) { state_[i] = SlotState::Writing; return i; }
        return kNone;
    }
    T* payload(std::uint32_t i) noexcept { return i < N && state_[i] == SlotState::Writing ? &slots_[i] : nullptr; }
    // Publishes a written slot. Rejects stale/duplicate generations (<= last published), 0,
    // > kMaxGeneration, or a slot not reserved for writing; the live configuration is unchanged.
    bool publish(std::uint32_t i, std::uint64_t generation) noexcept {
        if (i >= N || state_[i] != SlotState::Writing || generation == 0 || generation > kMaxGeneration ||
            generation <= lastPublished_) return false;
        gen_[i].value.store(generation, std::memory_order_release);
        state_[i] = SlotState::Published;
        lastPublished_ = generation;
        published_.store((generation << 8) | i, std::memory_order_release);
        return true;
    }
    // Returns a reserved-but-unpublished slot to the free list.
    void abandon(std::uint32_t i) noexcept { if (i < N && state_[i] == SlotState::Writing) state_[i] = SlotState::Free; }
    // Frees every published slot whose generation is older than the audio-acknowledged one.
    std::size_t reclaim() noexcept {
        const std::uint64_t acked = ack_.load(std::memory_order_acquire);
        std::size_t n = 0;
        for (std::uint32_t i = 0; i < N; ++i) {
            if (state_[i] != SlotState::Published) continue;
            if (gen_[i].value.load(std::memory_order_relaxed) < acked) {
                gen_[i].value.store(0, std::memory_order_relaxed);
                state_[i] = SlotState::Free;
                ++n;
            }
        }
        return n;
    }
    std::uint64_t lastPublished() const noexcept { return lastPublished_; }

    // ---- audio thread ----
    // Installs exactly (generation, slot) if it is newer than the installed one and the slot
    // still holds it. Returns the payload or nullptr (stale/unknown: installed config unchanged).
    const T* tryInstall(std::uint64_t generation, std::uint32_t slot) noexcept {
        if (slot >= N || generation == 0 || generation <= installed_) return nullptr;
        if (gen_[slot].value.load(std::memory_order_acquire) != generation) return nullptr;
        installed_ = generation;
        installedSlot_ = slot;
        ack_.store(generation, std::memory_order_release);
        return &slots_[slot];
    }
    // Installs the newest published generation if newer; returns current payload (may be null).
    const T* installLatest() noexcept {
        const std::uint64_t p = published_.load(std::memory_order_acquire);
        if (p != 0) (void)tryInstall(p >> 8, std::uint32_t(p & 0xFF));
        return current();
    }
    const T* current() const noexcept { return installedSlot_ == kNone ? nullptr : &slots_[installedSlot_]; }
    std::uint64_t installedGenerationAudio() const noexcept { return installed_; }

    // ---- any thread ----
    std::uint64_t acknowledgedGeneration() const noexcept { return ack_.load(std::memory_order_acquire); }

private:
    enum class SlotState : std::uint8_t { Free, Writing, Published };
    struct alignas(kCacheLine) Gen { std::atomic<std::uint64_t> value{0}; };
    std::unique_ptr<T[]> slots_;
    std::array<Gen, N> gen_{};
    std::array<SlotState, N> state_{};   // worker-private
    std::uint64_t lastPublished_ = 0;    // worker-private
    alignas(kCacheLine) std::atomic<std::uint64_t> published_{0};
    alignas(kCacheLine) std::atomic<std::uint64_t> ack_{0};
    alignas(kCacheLine) std::uint64_t installed_ = 0;  // audio-private
    std::uint32_t installedSlot_ = kNone;            // audio-private
};

}  // namespace cd::state
