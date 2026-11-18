// CircuitDrift control hub (P04 queue/snapshot interface). Framework-independent; depends on cd_core.
//
// ROLES AND OWNERSHIP (single owner per datum; no shared mutable state without an atomic handoff)
//   UI/message role  - any non-audio thread; calls are serialised by an internal mutex (audio never
//                      takes it). Owns: staged/committed configuration, the parameter event ring
//                      producer end, the config slot producer end, the generation counter.
//   Worker role      - any non-audio thread (serialised by a second mutex). Owns: capture requests,
//                      restore slot producer end, checkpoint slot consumer end, last-published
//                      checkpoint copy. Lock order: worker mutex -> UI mutex, never the reverse.
//   Audio role       - exactly ONE thread (the process callback). Owns the consumer ends of the
//                      event ring/config slot/restore slot, the producer end of the checkpoint slot,
//                      the SampleClock and the audio-private current configuration. Every audio-role
//                      method is noexcept, wait-free (bounded loops, atomics only), and performs no
//                      allocation, lock, I/O or logging. Audio never waits for another thread.
//
// DATA FLOW (all slots preallocated inside the hub at construction)
//   UI -> audio:     ConfigBlock via LatestSlot (coherent whole-config, coalescing latest-wins)
//                    ParamEvent via SpscRing (incremental automation, bounded 256) + producer-side
//                    coalescing table (see PRESSURE POLICY)
//   worker -> audio: restore request via LatestSlot; capture request via a monotonically increasing atomic
//   audio -> worker: CheckpointBlock via LatestSlot; acknowledgements via atomics
//
// GENERATIONS. A generation is the runtime-layout epoch (sample rate / oversampling / model /
// prepared circuit). beginGeneration() is called after a new layout is prepared. Everything stamped
// with an older generation is rejected on arrival at audio (config, events, restore) or by the worker
// (checkpoints), never applied. The provider-owned definition of what constitutes a layout change is
// an OPEN item with the solver owner; this hub only enforces ordering of whatever generation it is given.
//
// REVISIONS. Each commit() publishes one coherent configuration with revision R+1. An event is
// stamped with the revision current when enqueued; audio drops events older than its applied
// configuration revision (the configuration already contains their effect). Audio acquires events
// before the configuration, so an accepted event never precedes the configuration it was stamped with.
//
// BLOCK PROTOCOL (audio). beginBlock(frames) is called for EVERY host callback including zero-frame
// ones; it applies control state at the block boundary and never advances the clock. endBlock()
// advances the clock by `frames` and returns the completed tick count. Zero frames => no advance.
//   beginBlock -> [apply restore -> commitRestore] -> [apply config/events] -> [captureTarget ->
//   fill -> publishCapture] -> process audio -> endBlock.
//
// PRESSURE POLICY (deterministic, never blocks, never drops the latest value of a parameter)
//   * Config slot: never full; an unconsumed older commit is replaced by the newer (latest wins).
//   * Event ring (256): when full, the event is parked in a per-parameter table (one entry per
//     registry parameter, insertion order) holding only the LATEST value; later sends of the same
//     parameter overwrite it (eventsCoalesced). flushEvents()/the next sendParam() moves parked
//     values into the ring in first-parked order as space appears. A commit() discards parked
//     values (eventsSuperseded) because the committed configuration already contains them.
//     Intermediate values of a parameter that was parked are lost by design; the final value is not.
//   * Invalid input (unknown host ID, non-finite, outside [min,max], non-integer for Int/Bool/Enum)
//     is rejected and counted (inputRejected). Values are never clamped.
//   * Restore slot: latest wins; replaced requests report Superseded.
//   * Capture requests coalesce into one capture.
//   Limit: parked values are delivered only when the UI/worker thread calls into the hub again.
//
// STOPPED-AUDIO SAVE FALLBACK. save() ALWAYS returns the committed configuration (message-side
// authority; needs no audio cooperation). It asks audio for a fresh checkpoint and waits at most
// `timeout`; on timeout it returns the last coherent checkpoint published for the CURRENT generation
// (StaleFallback) or none (Unavailable). It never waits for an audio thread that may be stopped
// longer than `timeout` (0 => no wait).
//
// Limits (not claims): no engine, solver or thermal state is wired here; checkpoint array meaning
// and capacities (512/128/8) are placeholders pending G12; mapping to cd_state documents is future
// work; timed (sample-accurate) event offsets are not carried; MPMC is not supported.
#pragma once
#include <chrono>
#include <cstdint>
#include <memory>
#include "control/ControlTypes.h"
#include "control/SampleClock.h"

namespace cd::control {

class ControlHub {
public:
    ControlHub();  // allocates; construct on a non-audio thread. Large object: it lives on the heap.
    ~ControlHub();
    ControlHub(const ControlHub&) = delete;
    ControlHub& operator=(const ControlHub&) = delete;

    // ======== UI/message role (non-audio threads) ========
    // Stage a value for the next commit(). Rejects (returns false, counts) invalid input.
    bool stage(std::uint32_t hostId, double plainValue);
    // Publish staged configuration as one coherent revision. Returns the new revision.
    std::uint64_t commit();
    // Immediate automation value: updates staged+committed configuration and enqueues an event.
    bool sendParam(std::uint32_t hostId, double plainValue);
    // Move parked (coalesced) events into the ring as space allows. Returns number still parked.
    std::size_t flushEvents();
    // Start a new runtime-layout generation; republishes the committed configuration under it.
    // Returns the new generation (> previous). `layoutId` is an opaque provider-owned identity.
    std::uint64_t beginGeneration(std::uint64_t layoutId);
    std::uint64_t generation() const noexcept;
    ConfigBlock committedConfig() const;

    // ======== Worker role (non-audio threads) ========
    // Request that audio apply `cp` at a block boundary if the hub is still on `expectedGeneration`.
    // Returns a sequence number (> 0) or 0 if cp is malformed. cp.generation is overwritten.
    std::uint64_t requestRestore(const CheckpointBlock& cp, std::uint64_t expectedGeneration);
    RestoreStatus restoreStatus(std::uint64_t seq) const noexcept;
    // See STOPPED-AUDIO SAVE FALLBACK.
    void save(SaveResult& out, std::chrono::microseconds timeout);
    Diagnostics diagnostics() const noexcept;

    // ======== Audio role (single audio thread; all noexcept, wait-free) ========
    // Prepare the clock while audio is NOT running (host prepareToPlay). Not an audio-callback call.
    bool prepareClock(std::uint32_t sampleRateHz, std::uint32_t tickHz) noexcept;
    const BlockControl& beginBlock(std::uint32_t frames) noexcept;
    const ConfigBlock& currentConfig() const noexcept;  // revision 0 until the first config of a generation
    std::uint64_t audioGeneration() const noexcept;
    // Engine reports the outcome of applying block().restore (arrays + its own validation).
    void commitRestore(bool applied) noexcept;
    // Non-null only when block().captureRequested. Header (generation, config revision, clock) is
    // pre-filled at call time; engine fills arrays/counts then calls publishCapture().
    CheckpointBlock* captureTarget() noexcept;
    void publishCapture() noexcept;
    std::uint64_t endBlock() noexcept;
    const SampleClock& clock() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cd::control
