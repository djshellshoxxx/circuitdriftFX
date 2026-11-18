// CircuitDrift control: plain fixed-size payloads exchanged between roles (P04).
// All types are trivially copyable with compile-time bounds: nothing here allocates, so they can
// be copied inside wait-free slots. They deliberately do not depend on cd_state; an adapter maps
// them to/from state::StateDocument / EngineCheckpoint (not implemented here).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include "core/ParameterRegistry.h"

namespace cd::control {

inline constexpr std::size_t kConfigParamCount = core::kParamCount;
inline constexpr std::size_t kEventRingCapacity = 256;  // power of two
inline constexpr std::size_t kMaxElectrical = 512;      // doubles per checkpoint (bounded, G12 may revise)
inline constexpr std::size_t kMaxThermal = 128;
inline constexpr std::size_t kMaxRng = 8;

// Coherent full configuration: every registry parameter (registry order) in plain units.
// revision: increments on every commit (and generation bump); 0 means "no configuration yet".
// generation: runtime-layout epoch (see ControlHub::beginGeneration).
struct ConfigBlock {
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;
    std::array<double, kConfigParamCount> values{};
};

// Incremental automation event. Stamped with the generation/revision current when enqueued.
struct ParamEvent {
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;
    std::uint32_t hostId = 0;
    std::uint32_t pad = 0;
    double value = 0;
};

// Fixed-capacity dynamic-state snapshot. The engine owns the meaning of the arrays; control only
// moves them atomically as one unit together with the clock and the identifying headers.
struct CheckpointBlock {
    std::uint64_t generation = 0;       // layout epoch it was captured under
    std::uint64_t layoutId = 0;         // opaque provider-owned layout identity (informational)
    std::uint64_t configRevision = 0;   // configuration revision applied when captured
    std::uint64_t sampleIndex = 0;
    std::uint64_t clockRemainder = 0;   // SampleClock::remainder()
    std::uint32_t sampleRateHz = 0;
    std::uint32_t electricalCount = 0, thermalCount = 0, rngCount = 0, pad = 0;
    double electrical[kMaxElectrical] = {};
    double thermal[kMaxThermal] = {};
    std::uint64_t rng[kMaxRng] = {};
    bool countsValid() const noexcept {
        return electricalCount <= kMaxElectrical && thermalCount <= kMaxThermal && rngCount <= kMaxRng;
    }
};

enum class RestoreStatus : std::uint8_t {
    Pending = 0,
    Applied,
    RejectedStaleGeneration,
    RejectedIncompatible,  // sample rate/remainder mismatch or engine declined
    NotCommitted,          // engine never called commitRestore() for the block
    Superseded             // a newer restore request replaced it before audio saw it
};

enum class CheckpointStatus : std::uint8_t {
    Fresh,          // captured by audio after this save() began
    StaleFallback,  // audio did not answer in time: last coherent published checkpoint
    Unavailable     // none published for the current generation: configuration only
};

struct SaveResult {
    ConfigBlock config;  // ALWAYS valid: last committed configuration (message-side authority)
    CheckpointStatus status = CheckpointStatus::Unavailable;
    bool hasCheckpoint = false;
    bool checkpointMatchesConfig = false;  // checkpoint.configRevision == config.revision
    CheckpointBlock checkpoint;            // meaningful only if hasCheckpoint
};

// What audio must apply for one block. Pointers are valid until the next beginBlock().
struct BlockControl {
    std::uint32_t frames = 0;
    bool generationChanged = false;  // runtime layout changed: engine must re-prepare/adopt it
    bool configChanged = false;      // currentConfig() holds a new revision
    bool captureRequested = false;   // fill captureTarget() then publishCapture()
    std::uint32_t eventCount = 0;
    const ParamEvent* events = nullptr;       // accepted, FIFO, applied after config
    const CheckpointBlock* restore = nullptr; // validated request; apply then commitRestore()
};

struct Diagnostics {
    std::uint64_t inputRejected = 0;       // invalid id/value/non-finite/out-of-range (never clamped)
    std::uint64_t eventsDeferred = 0;      // event parked in producer coalescing table (ring full)
    std::uint64_t eventsCoalesced = 0;     // parked event overwritten by newer value of same param
    std::uint64_t eventsSuperseded = 0;    // parked events discarded because a commit covers them
    std::uint64_t eventsStale = 0;         // dropped by audio: old generation or revision
    std::uint64_t configsStale = 0;        // config dropped by audio: old generation
    std::uint64_t restoresRejected = 0;
    std::uint64_t capturesPublished = 0;
    std::uint64_t capturesDiscarded = 0;   // engine produced invalid counts
};

static_assert(std::is_trivially_copyable_v<ConfigBlock>);
static_assert(std::is_trivially_copyable_v<ParamEvent>);
static_assert(std::is_trivially_copyable_v<CheckpointBlock>);

}  // namespace cd::control
