// cd_control: per-block control orchestrator. Combines macros, creative drift, tempo breathing, key/scale
// lock, audio pitch source, harmonic targeting, transient detector and sidechain load into the
// core::ModulationBlock consumed by cd_engine (src/core/Modulation.h).
//
// One-way composition: reads a ParamValues snapshot, never writes it. Contributions are RAW block-rate
// targets (no 20 ms smoothing here); the engine smooths them in its declared domain. Creative modulation
// is not physics (spec): it never touches kelvin, rail voltage or electrical state.
//
// Threading:
//  - prepare()/reset()/setHarmonicMap()/setSidechainBudgetA() : non-audio thread, while the audio thread
//    is not inside processBlock (setHarmonicMap is an atomic pointer publish; the caller keeps the map
//    alive until harmonicMapInUse(slot) reports it or a newer one).
//  - processBlock() : audio thread only, noexcept, no allocation/locks/I/O. frames==0 advances no clock
//    (outputs reflect current parameters and the last block's state). frames > maxBlock is clamped and
//    flagged in status().blockClamped.
//  - runPitchWorker() : worker thread (single), drains the pitch ring and analyses windows.
//  - status() : audio thread (same thread as processBlock); the runtime may copy it for the UI.
// Output pointers in ModulationBlock refer to processor-owned buffers valid until the next processBlock.
#pragma once
#include <atomic>
#include <cstdint>
#include <vector>
#include "control/ControlTypes.h"
#include "control/CreativeDrift.h"
#include "control/HarmonicTargeting.h"
#include "control/KeyTracker.h"
#include "control/PitchDetector.h"
#include "control/ScaleQuantizer.h"
#include "control/SidechainLoad.h"
#include "control/TempoBreathing.h"
#include "control/TransientDetector.h"
#include "core/Modulation.h"
#include "core/ParameterRegistry.h"

namespace cd::control {

class ControlProcessor {
public:
    ControlProcessor() = default;
    ControlProcessor(const ControlProcessor&) = delete;
    ControlProcessor& operator=(const ControlProcessor&) = delete;

    void prepare(double sampleRate, int maxBlock, uint64_t instanceSeed);
    void reset();

    void processBlock(const core::ParamValues& params, const ControlContext& ctx, const AudioView& mainIn,
                      const AudioView* sidechainIn, core::ModulationBlock& out) noexcept;

    void runPitchWorker() noexcept { pitch_.run(); }

    void setHarmonicMap(int slot, const HarmonicMap* map) noexcept;
    const HarmonicMap* harmonicMapInUse(int slot) const noexcept;
    void setSidechainBudgetA(double amps) noexcept { budgetA_.store(amps, std::memory_order_relaxed); }
    void setReferenceTuning(double a4Hz) noexcept { a4_.store(a4Hz, std::memory_order_relaxed); }

    const ControlStatus& status() const noexcept { return status_; }
    PitchDetector& pitchDetector() noexcept { return pitch_; }
    const CreativeDrift& drift(int slot) const noexcept { return drift_[slot & 1]; }
    CreativeDrift& drift(int slot) noexcept { return drift_[slot & 1]; }

private:
    struct SlotState {
        ResonanceQuantizer quant;
        HarmonicTargeting harmonic;
        double lockW = 0.0;           // key lock blend 0..1 (50 ms)
        double delayHold = 1.0;       // 1 free .. 0 held by scale lock (100 ms ramp)
        std::atomic<const HarmonicMap*> pendingMap{nullptr};
        std::atomic<const HarmonicMap*> mapInUse{nullptr};
        // cached outputs for frames==0 calls
        double cutoffOct = 0, delayMs = 0, dBias = 0, dDrive = 0, lastQuantOff = 0;
    };

    bool prepared_ = false;
    double fs_ = 48000.0;
    int maxBlock_ = 0;
    CreativeDrift drift_[2];
    TempoBreathing tempo_;
    KeyTracker key_;
    PitchDetector pitch_;
    TransientDetector transient_;
    SidechainLoad sidechain_;
    SlotState slot_[2];
    std::vector<double> loadBuf_, transBuf_, mono_;
    std::atomic<double> budgetA_{0.0};
    std::atomic<double> a4_{440.0};
    ControlStatus status_{};
    int lastKeySource_ = 0;
    TempoOutput lastTempo_{};
};

}  // namespace cd::control
