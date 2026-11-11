// CircuitDrift runtime: per-prepared-engine control overlays wired into Engine (resume packet 1,
// remaining stages): transient/sidechain detectors (CD-F15/F16), tempo clock (CD-F13), seeded
// creative motion + pitch-drift delay (D09) and component fault channels (CD-F19).
// Owner: cd_runtime. Configuration types (OverlayConfig) are read by Engine::buildPrepared off the
// audio thread; OverlayRuntime is trivially copyable and is part of the engine snapshot layout.
// Every number here that stands for hardware (I_budget, fault bounds) is configuration supplied by
// a manifest; the built-in SYNTHETIC fixtures declare none (G01 open), so their defaults are 0 /
// labelled fixture values and never a physical claim.
#pragma once
#include <array>
#include <cstdint>
#include <type_traits>
#include "blueprint/Faults.h"
#include "control/CreativeMotion.h"
#include "control/Dynamics.h"
#include "control/Macros.h"
#include "control/TempoClock.h"

namespace cd::runtime {

inline constexpr int kMaxFaultSlots = 4;

// One fault-capable component of the prepared built-in netlist. `element` indexes
// CircuitConfig::netlist.elements; availability is decided by blueprint::faultAvailable from the
// element's equation family (no tube family exists, so microphonic faults are always unavailable)
// and from what the manifest exposes (ElectrolyticLeaky needs a leakage parameter: unavailable).
struct FaultSlotConfig {
    bool used = false;
    int element = -1;
    // ESR/leakage/bias-span fields only: the R/C fields (nominalR/openR, nominalC/minC) are derived
    // from the element's netlist value and manifest bounds at prepare (see Engine.h "Faults").
    blueprint::FaultBounds bounds{};
    blueprint::FaultOverlay overlay{};    // initial overlay (default: disabled)
};

struct OverlayConfig {
    // CD-F15: transient.preserve amount 0..1 (P = amount*b).
    double transientAmount = 0;
    // CD-F16: sidechain.source / depth 0..2 / hpf 20..300 Hz; I_budget [A] from the manifest
    // (synthetic fixtures declare no supply budget: 0 => I_ext == 0 whatever the depth).
    control::SidechainSource sidechainSource = control::SidechainSource::Main;
    double sidechainDepth = 0, sidechainHpfHz = 40, sidechainBudgetAmps = 0;
    // CD-F13: tempo.enabled / division / modifier / depth (creative sinusoidal drift depth 0..1).
    bool tempoEnabled = false;
    control::Division tempoDivision = control::Division::Quarter;
    control::Modifier tempoModifier = control::Modifier::Straight;
    double tempoDepth = 0;
    // D09: pitchDrift.enabled (registry default off). The delay runs only when this is set, the
    // macro bindings declare Drift and the engine is not Zero Latency; it adds its base delay to
    // the declared latency (prepare-time latency handshake).
    bool pitchDriftEnabled = false;
    std::uint64_t motionSeed = 0x43445f4d4f54494full;   // instance seed for deriveSeed()
    std::array<FaultSlotConfig, kMaxFaultSlots> faults{};
};

// Current overlay parameter targets (trivially copyable; restored by Engine::reset()).
struct OverlayParams {
    double transientAmount = 0, sidechainDepth = 0, sidechainHpfHz = 40, sidechainBudgetAmps = 0, tempoDepth = 0;
    std::uint32_t sidechainSource = 0, tempoDivision = 3, tempoModifier = 0, tempoEnabled = 0;
    blueprint::FaultOverlay faults[kMaxFaultSlots]{};
};

struct OverlayRuntime {
    OverlayParams prm{};
    control::TransientDetector transient;
    control::SidechainLoadDetector sidechain;   // configured with depth 1, I_budget 1: returns clamp(E/E_ref,0,4)
    control::TempoClock tempo;
    control::CreativeMotion motion;
    blueprint::FaultChannel faults[kMaxFaultSlots];
    blueprint::FaultOutputs faultOut[kMaxFaultSlots]{};
    control::FaultOverlay hook{};               // fault hook fed to control::evaluate
    std::int64_t hookActive = 0;
    double b = 0, P = 0, iExt = 0, iLoad = 0, tempoDrift = 0, modQ = 0, evalQ = 0;   // evalQ: q of the last evaluate()
    std::int64_t tempoResets = 0, faultApplied = 0, faultRejected = 0, faultResets = 0;
};
static_assert(std::is_trivially_copyable_v<OverlayRuntime>, "OverlayRuntime is part of the snapshot layout");

}  // namespace cd::runtime
