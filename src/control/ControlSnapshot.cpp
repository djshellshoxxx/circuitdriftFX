#include "control/ControlProcessor.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace cd::control {
namespace {
bool inRange(double value, double low, double high) noexcept {
    return std::isfinite(value) && value >= low && value <= high;
}

constexpr double kMaxBudgetAmps = std::numeric_limits<double>::max() / 8.0;

double effectiveBudget(double raw) noexcept {
    // This pending setting is only consumed through SidechainLoad::setBudgetAmps.
    // Canonicalizing here preserves that future effect, including invalid inputs,
    // without changing the detector's separate, previously applied budget.
    return std::isfinite(raw) && raw > 0.0 ? std::min(raw, kMaxBudgetAmps) : 0.0;
}
}  // namespace

ControlSnapshotResult ControlProcessor::snapshotSupport() const noexcept {
    if (!prepared_) return ControlSnapshotResult::NotPrepared;
    if (lastKeySource_ == int(KeySource::Audio) || audioPitchActive_)
        return ControlSnapshotResult::UnsupportedAudioPitch;
    for (const auto& slot : slot_) {
        if (slot.pendingMap.load(std::memory_order_acquire) ||
            slot.mapInUse.load(std::memory_order_acquire) || slot.harmonic.available())
            return ControlSnapshotResult::UnsupportedHarmonicMap;
    }
    return ControlSnapshotResult::Ok;
}

ControlSnapshotResult ControlProcessor::captureSnapshot(ControlSnapshot& destination) const noexcept {
    if (const auto result = snapshotSupport(); result != ControlSnapshotResult::Ok) return result;
    ControlSnapshot candidate;
    candidate.sampleRate = fs_;
    candidate.maxBlock = maxBlock_;
    candidate.instanceSeed = instanceSeed_;
    for (int i = 0; i < 2; ++i) {
        const auto& live = slot_[i];
        auto& saved = candidate.slots[std::size_t(i)];
        saved.drift = drift_[i].save();
        saved.quantizer = live.quant.save();
        saved.lockWeight = live.lockW;
        saved.delayHold = live.delayHold;
        saved.cutoffOct = live.cutoffOct;
        saved.delayMs = live.delayMs;
        saved.lastQuantizedOffset = live.lastQuantOff;
    }
    candidate.tempo = tempo_.save();
    candidate.key = key_.save();
    candidate.transient = transient_.save();
    candidate.sidechain = sidechain_.save();
    candidate.sidechainBudgetAmps = effectiveBudget(budgetA_.load(std::memory_order_relaxed));
    candidate.a4Hz = a4_.load(std::memory_order_relaxed);
    candidate.status = status_;
    candidate.keySource = lastKeySource_;
    candidate.audioPitchActive = audioPitchActive_;
    const auto result = canRestoreSnapshot(candidate);
    if (result == ControlSnapshotResult::Ok) destination = candidate;
    return result;
}

ControlSnapshotResult ControlProcessor::canRestoreSnapshot(const ControlSnapshot& s) const noexcept {
    if (const auto result = snapshotSupport(); result != ControlSnapshotResult::Ok) return result;
    if (s.version != ControlSnapshot::kVersion || s.sampleRate != fs_ ||
        s.maxBlock != maxBlock_ || s.instanceSeed != instanceSeed_)
        return ControlSnapshotResult::Incompatible;
    if (s.keySource < int(KeySource::Manual) || s.keySource > int(KeySource::Audio) ||
        s.status.pitchState > PitchState::Acquired)
        return ControlSnapshotResult::InvalidState;
    if (s.keySource == int(KeySource::Audio) || s.audioPitchActive ||
        s.status.pitchState != PitchState::None)
        return ControlSnapshotResult::UnsupportedAudioPitch;
    if (s.status.harmonicAvailable[0] || s.status.harmonicAvailable[1])
        return ControlSnapshotResult::UnsupportedHarmonicMap;

    if (!inRange(s.sidechainBudgetAmps, 0, kMaxBudgetAmps) || !std::isfinite(s.a4Hz) || s.a4Hz <= 0 ||
        !inRange(s.status.tempoPhase, 0, 1) || !inRange(s.status.referenceNote, -32768, 32768) ||
        s.status.keyState > KeyState::Returning || s.status.pitchHz != 0 || s.status.pitchConfidence != 0 ||
        (s.status.sidechainConnected && s.status.sidechainMissing) ||
        (s.status.pitchDriftHeld && !s.status.scaleLockActive) ||
        s.status.tempoActive != s.tempo.wasEnabled || s.status.tempoFallback != s.tempo.output.fallback ||
        s.status.tempoPhase != s.tempo.output.phase ||
        s.status.keyState != (s.status.scaleLockActive ? s.key.state : KeyState::Off))
        return ControlSnapshotResult::InvalidState;

    // Every leaf's preparation is part of the aggregate rate identity. The two
    // independent drift clocks need not agree: drift() permits separate advances.
    if (s.tempo.sampleRate != fs_ || s.key.sampleRate != fs_ || s.transient.sampleRate != fs_ ||
        s.sidechain.sampleRate != fs_ || !tempo_.canRestore(s.tempo) || !key_.canRestore(s.key) ||
        !transient_.canRestore(s.transient) || !sidechain_.canRestore(s.sidechain))
        return ControlSnapshotResult::InvalidState;
    constexpr double cutoffLimit = 2.0 + 16 * std::numeric_limits<double>::epsilon();
    for (int i = 0; i < 2; ++i) {
        const auto& saved = s.slots[std::size_t(i)];
        if (saved.drift.sampleRate != fs_ || !drift_[i].canRestore(saved.drift) ||
            !slot_[i].quant.canRestore(saved.quantizer) ||
            !inRange(saved.lockWeight, 0, 1) || !inRange(saved.delayHold, 0, 1) ||
            !inRange(saved.cutoffOct, -cutoffLimit, cutoffLimit) ||
            !inRange(saved.delayMs, -CreativeDrift::kMaxDelayMs, CreativeDrift::kMaxDelayMs) ||
            !inRange(saved.lastQuantizedOffset, -2, 2))
            return ControlSnapshotResult::InvalidState;
    }
    return ControlSnapshotResult::Ok;
}

ControlSnapshotResult ControlProcessor::restoreSnapshot(const ControlSnapshot& s) noexcept {
    // Validate the entire frozen candidate before the first write. Under the
    // single-owner contract each subsequent leaf admission has the same inputs
    // and unchanged preparation, so there is no rejecting branch during commit.
    const auto result = canRestoreSnapshot(s);
    if (result != ControlSnapshotResult::Ok) return result;
    for (int i = 0; i < 2; ++i) {
        const auto& saved = s.slots[std::size_t(i)];
        auto& live = slot_[i];
        drift_[i].restore(saved.drift);
        (void)live.quant.restore(saved.quantizer);
        live.lockW = saved.lockWeight;
        live.delayHold = saved.delayHold;
        live.cutoffOct = saved.cutoffOct;
        live.delayMs = saved.delayMs;
        live.lastQuantOff = saved.lastQuantizedOffset;
        live.dBias = live.dDrive = 0;
        live.harmonic = HarmonicTargeting{};
    }
    (void)tempo_.restore(s.tempo);
    (void)key_.restore(s.key);
    (void)transient_.restore(s.transient);
    (void)sidechain_.restore(s.sidechain);
    lastTempo_ = s.tempo.output;
    budgetA_.store(s.sidechainBudgetAmps, std::memory_order_relaxed);
    a4_.store(s.a4Hz, std::memory_order_relaxed);
    status_ = s.status;
    lastKeySource_ = s.keySource;
    audioPitchActive_ = s.audioPitchActive;
    return ControlSnapshotResult::Ok;
}

}  // namespace cd::control
