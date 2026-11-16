// CircuitDrift state: audio->worker coherent engine snapshot exchange (P04 snapshot interface;
// 05-state "capture by preallocated snapshot request at a block boundary").
// Owner: cd_state.
// Ownership/threading:
//  * Constructor allocates three fixed-size slots (header + payloadBytes each). Non-audio.
//  * requestCapture(): any non-audio thread (atomic counter; never blocks).
//  * Audio thread (single writer): captureRequested(), writeSlot(), publish() at a block
//    boundary. All noexcept, allocation/lock free; publish is one atomic exchange.
//  * readLatest()/hostSave(): any non-audio thread, possibly concurrently. Readers serialise on
//    a reader-only mutex (the audio thread never touches it) and copy the last coherent
//    published slot. They never wait for the audio thread, so a host save with stopped audio
//    returns the last published snapshot, or NoCheckpoint when none was ever published.
// Triple buffer: audio owns `back`, readers own `front`, `middle` is exchanged atomically with
// a fresh bit; the slot being written is never the slot being read (data-race free).
// A snapshot is only restorable into a runtime with identical provider layout (id, version,
// byte size), sample rate, oversampling and model identity; anything else is a configuration-
// only restore (RestoreDecision::ResetWarm) and the live state is left untouched.
// Exact integrated continuation is NOT claimed: providers must still define complete capture
// (filter/delay histories, RNG, thermal clock phase, fallback state) for their layouts.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>
#include "state/StateDocument.h"

namespace cd::state {

// Versioned provider layout: a payload is meaningful only to a reader with the same triple.
struct LayoutDescriptor {
    std::uint32_t layoutId = 0;   // provider-assigned identity of the state layout (0 = none)
    std::uint32_t version = 0;    // bumped whenever field order/meaning changes
    std::uint32_t byteSize = 0;   // exact payload byte count
    bool operator==(const LayoutDescriptor&) const = default;
};

// Runtime identity a snapshot must match to be restored (POD; copied into every header).
struct SnapshotIdentity {
    LayoutDescriptor layout;
    double sampleRate = 0;
    std::uint32_t oversampling = 1;
    std::uint64_t modelId = 0;    // provider hash of engine model version + revision + topology
    bool operator==(const SnapshotIdentity&) const = default;
};

struct SnapshotHeader {
    SnapshotIdentity identity;
    std::uint64_t sampleIndex = 0;      // engine sample clock at the block boundary
    double clockRemainder = 0;          // fractional control-clock phase (e.g. 1 kHz thermal)
    std::uint64_t configRevision = 0;   // installed configuration generation
    std::uint64_t publishSequence = 0;  // 1,2,3... assigned by publish()
    std::uint64_t servedRequest = 0;    // capture request counter value served by this publish
};

// Owned copy produced on a reader thread.
struct SavedSnapshot {
    bool valid = false;
    SnapshotHeader header;
    std::vector<std::byte> payload;
};

class SnapshotExchange {
public:
    // identity.layout.byteSize defines the fixed payload size (> 0, <= 64 MiB).
    explicit SnapshotExchange(const SnapshotIdentity& identity);
    SnapshotExchange(const SnapshotExchange&) = delete;
    SnapshotExchange& operator=(const SnapshotExchange&) = delete;

    const SnapshotIdentity& identity() const noexcept { return identity_; }

    // ---- any non-audio thread ----
    // Returns the request token; the request is served when a header has servedRequest >= token.
    std::uint64_t requestCapture() noexcept;

    // ---- audio thread only ----
    bool captureRequested() const noexcept;
    // Back slot payload (exactly layout.byteSize bytes) to fill at a block boundary. Requests
    // issued before this call are served by the following publish().
    std::span<std::byte> writePayload() noexcept;
    // Publishes the back slot with this header (identity/sequence/served fields are set here).
    void publish(std::uint64_t sampleIndex, double clockRemainder, std::uint64_t configRevision) noexcept;

    // ---- any non-audio thread (allocates the copy) ----
    // Copies the last coherent published snapshot; out.valid=false when none exists.
    void readLatest(SavedSnapshot& out) const;
    std::uint64_t publishedCount() const noexcept { return published_.load(std::memory_order_acquire); }

private:
    struct Slot {
        SnapshotHeader header;
        std::unique_ptr<std::byte[]> payload;
    };
    static constexpr std::uint8_t kFresh = 0x4;
    SnapshotIdentity identity_;
    Slot slots_[3];
    alignas(64) std::atomic<std::uint64_t> requested_{0};
    alignas(64) mutable std::atomic<std::uint8_t> middle_{1};  // readers exchange under readerMutex_
    std::atomic<std::uint64_t> published_{0};
    alignas(64) std::uint8_t back_ = 0;          // audio-private
    std::uint64_t sequence_ = 0;                 // audio-private
    std::uint64_t served_ = 0;                   // audio-private
    std::uint64_t serving_ = 0;                  // audio-private: request seen at writePayload()
    mutable std::mutex readerMutex_;             // readers only
    mutable std::uint8_t front_ = 2;             // guarded by readerMutex_
    mutable bool hasFront_ = false;              // guarded by readerMutex_
};

// Host save: configuration is mandatory; the dynamic part is the last coherent snapshot if any.
struct HostSaveResult {
    RestoreDecision decision = RestoreDecision::NoCheckpoint;  // Restore = snapshot attached
    std::string reason;
    std::string configuration;  // canonical configuration text supplied by the caller
    SavedSnapshot snapshot;
};
// Never waits for the audio thread. Non-audio; allocates.
HostSaveResult hostSave(const SnapshotExchange& exchange, std::string configuration);

// Decide whether a saved snapshot may be restored into a runtime with identity `rt`.
// NoCheckpoint: none saved; ResetWarm: layout/Fs/OS/model mismatch (configuration-only);
// Restore: identical identity and payload size.
RestoreDecision decideSnapshotRestore(const SavedSnapshot& saved, const SnapshotIdentity& rt, std::string& reason);
// Copies the payload into liveState only when decideSnapshotRestore returns Restore and
// liveState.size() == payload size; otherwise liveState is left unchanged. Non-audio.
RestoreDecision restoreSnapshot(const SavedSnapshot& saved, const SnapshotIdentity& rt,
                                std::span<std::byte> liveState, std::string& reason);

}  // namespace cd::state
