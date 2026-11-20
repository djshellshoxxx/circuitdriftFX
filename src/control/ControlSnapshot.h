// Fixed-size, in-memory control continuation. This is a typed C++ value, not a
// wire format or an engine/host-session checkpoint. Parameters and future inputs
// remain the caller's responsibility; audio pitch and harmonic maps are excluded.
#pragma once
#include <array>
#include <cstdint>
#include <type_traits>
#include "control/ControlTypes.h"
#include "control/CreativeDrift.h"
#include "control/KeyTracker.h"
#include "control/ScaleQuantizer.h"
#include "control/SidechainLoad.h"
#include "control/TempoBreathing.h"
#include "control/TransientDetector.h"

namespace cd::control {

enum class ControlSnapshotResult : uint8_t {
    Ok,
    NotPrepared,
    Incompatible,
    UnsupportedAudioPitch,
    UnsupportedHarmonicMap,
    InvalidState
};

struct ControlSnapshot {
    // Bump when either the state contract or a continuation algorithm changes.
    static constexpr uint32_t kVersion = 1;
    uint32_t version = kVersion;
    double sampleRate = 0;
    int maxBlock = 0;
    uint64_t instanceSeed = 0;  // also identifies the prepared reset origin

    struct Slot {
        CreativeDrift::State drift{};
        ResonanceQuantizer::State quantizer{};
        double lockWeight = 0, delayHold = 1;
        double cutoffOct = 0, delayMs = 0, lastQuantizedOffset = 0;
    };
    std::array<Slot, 2> slots{};
    TempoBreathing::State tempo{};
    KeyTracker::State key{};
    TransientDetector::State transient{};
    SidechainLoad::State sidechain{};

    // Effective pending setting, normalized exactly as SidechainLoad normalizes
    // its input. It can differ from sidechain.budgetAmps until the next block.
    double sidechainBudgetAmps = 0;
    double a4Hz = 440;
    ControlStatus status{};
    int keySource = int(KeySource::Manual);
    bool audioPitchActive = false;
};

static_assert(std::is_trivially_copyable_v<ControlSnapshot>);
static_assert(std::is_standard_layout_v<ControlSnapshot>);
static_assert(sizeof(ControlSnapshot) <= 4096, "control capture must remain fixed and small");

}  // namespace cd::control
