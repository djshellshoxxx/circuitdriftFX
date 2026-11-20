#include "control/ControlProcessor.h"
#include <algorithm>
#include <cmath>
#include "control/MacroMapper.h"
#include "core/CutoffDomain.h"

namespace cd::control {
namespace {
using core::indexOf;
constexpr auto kAge = indexOf("macro.age"), kSag = indexOf("macro.sag"), kDriftM = indexOf("macro.drift"),
               kWarmth = indexOf("macro.warmth"), kNoise = indexOf("analogNoise.enabled"),
               kPitchDrift = indexOf("pitchDrift.enabled"), kZeroLat = indexOf("quality.zeroLatency"),
               kScSource = indexOf("sidechain.source"), kScDepth = indexOf("sidechain.depth"),
               kScHpf = indexOf("sidechain.hpf"), kTransient = indexOf("transient.preserve"),
               kKeySource = indexOf("key.source"), kKeyEnabled = indexOf("key.enabled"),
               kKeyRoot = indexOf("key.root"), kKeyScale = indexOf("key.scale"), kKeyMask = indexOf("key.mask"),
               kTempoEn = indexOf("tempo.enabled"), kTempoDiv = indexOf("tempo.division"),
               kTempoMod = indexOf("tempo.modifier"), kTempoDepth = indexOf("tempo.depth"),
               kHTarget = indexOf("harmonics.target"), kHAmount = indexOf("harmonics.amount");
constexpr std::size_t kBiasIdx[2] = {indexOf("slot.a.stage.bias"), indexOf("slot.b.stage.bias")};
constexpr std::size_t kDriveIdx[2] = {indexOf("slot.a.stage.drive"), indexOf("slot.b.stage.drive")};
constexpr std::size_t kFreqIdx[2] = {indexOf("slot.a.filter.lpfFrequency"), indexOf("slot.b.filter.lpfFrequency")};
static_assert(kAge < core::kParamCount && kSag < core::kParamCount && kDriftM < core::kParamCount &&
              kWarmth < core::kParamCount && kNoise < core::kParamCount && kPitchDrift < core::kParamCount &&
              kZeroLat < core::kParamCount && kScSource < core::kParamCount && kScDepth < core::kParamCount &&
              kScHpf < core::kParamCount && kTransient < core::kParamCount && kKeySource < core::kParamCount &&
              kKeyEnabled < core::kParamCount && kKeyRoot < core::kParamCount && kKeyScale < core::kParamCount &&
              kKeyMask < core::kParamCount && kTempoEn < core::kParamCount && kTempoDiv < core::kParamCount &&
              kTempoMod < core::kParamCount && kTempoDepth < core::kParamCount && kHTarget < core::kParamCount &&
              kHAmount < core::kParamCount && kBiasIdx[0] < core::kParamCount && kBiasIdx[1] < core::kParamCount &&
              kDriveIdx[0] < core::kParamCount && kDriveIdx[1] < core::kParamCount &&
              kFreqIdx[0] < core::kParamCount && kFreqIdx[1] < core::kParamCount,
              "registry id missing");

double num(double v, double fallback = 0.0) noexcept { return std::isfinite(v) ? v : fallback; }
double unit(double v) noexcept { return std::clamp(num(v), 0.0, 1.0); }
bool flag(double v) noexcept { return num(v) >= 0.5; }
int enumIdx(double v, int hi) noexcept { return int(std::lround(std::clamp(num(v), 0.0, double(hi)))); }
double referencePitch(double hz, double a4Hz) noexcept {
    const double ratio = hz / a4Hz;
    // Positive finite tuning can still overflow the ratio (e.g. a subnormal A4).
    const double octaves = std::isfinite(ratio) && ratio > 0.0 ? std::log2(ratio)
                                                             : std::log2(hz) - std::log2(a4Hz);
    return 69.0 + 12.0 * octaves;
}
double admittedTargetPitch(double target, const core::CutoffDomain& domain,
                           double a4Hz, uint16_t mask, int root) noexcept {
    // Positive finite A4 and the prepared filter domain (500..80000 Hz) keep
    // both pitch bounds inside +/-14000, including subnormal reference tuning.
    const int first = int(std::ceil(referencePitch(domain.minHz, a4Hz)));
    const int last = int(std::floor(referencePitch(domain.maxHz, a4Hz)));
    int low = first, high = last;
    for (int i = 0; i < 12; ++i) {
        if (scale::isAllowed(mask, root, first + i)) { low = first + i; break; }
    }
    for (int i = 0; i < 12; ++i) {
        if (scale::isAllowed(mask, root, last - i)) { high = last - i; break; }
    }
    // The domain spans at least two octaves, so low <= high for every nonzero
    // scale. Keeping the original pitch between these allowed endpoints retains
    // midpoint ties and 0.15-semitone hysteresis throughout the admitted range.
    return std::clamp(target, double(low), double(high));
}
double approach(double cur, double target, double maxStep) noexcept {
    return cur + std::clamp(target - cur, -maxStep, maxStep);
}
}  // namespace

void ControlProcessor::prepare(double sampleRate, int maxBlock, uint64_t instanceSeed) {
    prepared_ = false;
    fs_ = std::isfinite(sampleRate) && sampleRate >= 8000.0 && sampleRate <= 768000.0 ? sampleRate : 48000.0;
    maxBlock_ = std::max(0, maxBlock);
    loadBuf_.assign(std::size_t(maxBlock_) + 1, 0.0);
    transBuf_.assign(std::size_t(maxBlock_) + 1, 0.0);
    mono_.assign(std::size_t(maxBlock_) + 1, 0.0);
    for (uint32_t s = 0; s < 2; ++s) {
        drift_[s].prepare(fs_, instanceSeed, s);
        initialDrift_[s] = drift_[s].save();
    }
    tempo_.prepare(fs_);
    key_.prepare(fs_);
    pitch_.prepare(fs_);
    transient_.prepare(fs_);
    sidechain_.prepare(fs_);
    for (auto& st : slot_) {
        st.quant.reset();
        st.harmonic.reset();
        st.lockW = 0.0;
        st.delayHold = 1.0;
        st.cutoffOct = st.delayMs = st.dBias = st.dDrive = st.lastQuantOff = 0.0;
    }
    status_ = ControlStatus{};
    lastKeySource_ = 0;
    audioPitchActive_ = false;
    lastTempo_ = TempoOutput{};
    instanceSeed_ = instanceSeed;
    prepared_ = true;
}

void ControlProcessor::reset() {
    if (!prepared_) return;
    resetAudioState();
    pitch_.reset();
}

void ControlProcessor::resetRealtime() noexcept {
    if (!prepared_) return;
    resetAudioState();
    pitch_.markDiscontinuity();
}

void ControlProcessor::resetAudioState() noexcept {
    // The prepared initial snapshots avoid coefficient/seed preparation here.
    for (uint32_t s = 0; s < 2; ++s) drift_[s].restore(initialDrift_[s]);
    key_.reset();
    tempo_.reset();
    transient_.reset();
    sidechain_.reset();
    for (auto& st : slot_) {
        st.quant.reset();
        st.harmonic.reset();
        st.lockW = 0.0;
        st.delayHold = 1.0;
        st.cutoffOct = st.delayMs = st.dBias = st.dDrive = st.lastQuantOff = 0.0;
    }
    status_ = ControlStatus{};
    lastKeySource_ = 0;
    audioPitchActive_ = false;
    lastTempo_ = TempoOutput{};
}

void ControlProcessor::setHarmonicMap(int slot, const HarmonicMap* map) noexcept {
    slot_[slot & 1].pendingMap.store(map, std::memory_order_release);
}
const HarmonicMap* ControlProcessor::harmonicMapInUse(int slot) const noexcept { return slot_[slot & 1].mapInUse.load(std::memory_order_acquire); }

void ControlProcessor::setReferenceTuning(double a4Hz) noexcept {
    a4_.store(std::isfinite(a4Hz) && a4Hz > 0.0 ? a4Hz : 440.0, std::memory_order_relaxed);
}

void ControlProcessor::processBlock(const core::ParamValues& p, const ControlContext& ctx, const AudioView& mainIn,
                                    const AudioView* scIn, core::ModulationBlock& out) noexcept {
    out = core::ModulationBlock{};
    if (!prepared_) return;
    status_.blockClamped = ctx.frames > maxBlock_ || ctx.frames < 0;
    const int frames = std::clamp(ctx.frames, 0, maxBlock_);
    const double dt = double(frames) / fs_;
    const double a4 = a4_.load(std::memory_order_relaxed);

    // 1. Macros (pure, global).
    core::SlotModulation base;
    MacroInputs mi;
    mi.age = p[kAge]; mi.sag = p[kSag]; mi.drift = p[kDriftM]; mi.warmth = p[kWarmth];
    mi.analogNoiseEnabled = flag(p[kNoise]);
    applyMacros(mi, base);
    const double d = unit(p[kDriftM]);

    // 2. Tempo breathing.
    TempoInput ti;
    ti.enabled = flag(p[kTempoEn]);
    ti.division = enumIdx(p[kTempoDiv], 5);
    ti.modifier = enumIdx(p[kTempoMod], 2);
    ti.depth = unit(p[kTempoDepth]);
    ti.bpm = ctx.bpm;
    ti.ppq = ctx.ppq;
    ti.timeSigNum = ctx.timeSigNum;
    ti.timeSigDen = ctx.timeSigDen;
    ti.playing = ctx.playing;
    lastTempo_ = tempo_.process(ti, frames);

    // 3. Reference note source (MIDI / Audio / Manual).
    const bool keyEnabled = flag(p[kKeyEnabled]);
    const int keySource = enumIdx(p[kKeySource], 2);
    if (keySource != lastKeySource_) {
        key_.reset();
        lastKeySource_ = keySource;
    }
    const bool audioPitchActive = keySource == int(KeySource::Audio) && keyEnabled;
    if (audioPitchActive != audioPitchActive_) {
        // The accepted-input clock pauses while audio tracking is inactive.
        // Invalidate its old result/history instead of treating it as fresh on return.
        pitch_.markDiscontinuity();
        audioPitchActive_ = audioPitchActive;
    }
    PitchResult pr{};
    if (audioPitchActive) {
        if (frames > 0) {
            for (int i = 0; i < frames; ++i) mono_[std::size_t(i)] = 0.0;
            int nch = 0;
            for (int c = 0; c < mainIn.numChannels && mainIn.channels; ++c) {
                if (!mainIn.channels[c]) continue;
                ++nch;
                for (int i = 0; i < frames; ++i) mono_[std::size_t(i)] += mainIn.channels[c][i];
            }
            if (nch > 1) for (int i = 0; i < frames; ++i) mono_[std::size_t(i)] /= nch;
            pitch_.push(mono_.data(), frames);
        }
        pr = pitch_.latest();
    }
    const bool pitchUsable = pitch_.isFresh(pr) && (pr.state == PitchState::Acquired || pr.held);
    if (keySource == int(KeySource::Midi) && ctx.frames >= 0) {
        if (frames > 0) key_.advanceMidi(ctx.midi, frames);
        else {
            // Zero-frame flushes can release held notes. Future-offset events are
            // outside this call, and no elapsed-time or pitch-worker state changes.
            for (const auto& event : ctx.midi) {
                if (event.frame != 0) break;
                key_.advanceMidi({&event, 1}, 0);
            }
        }
    } else if (frames > 0) {
        if (keySource == int(KeySource::Audio) && keyEnabled)
            key_.advanceAudio(pitchUsable, pitchUsable ? referencePitch(pr.frequencyHz, a4) : 0.0, frames);
        else key_.advanceManual(frames);
    }
    status_.pitchState = pr.state;
    status_.pitchHz = pitchUsable ? pr.frequencyHz : 0.0;
    status_.pitchConfidence = pr.state == PitchState::Acquired ? pr.confidence : (pr.state == PitchState::Low ? std::min(pr.confidence, 0.5) : 0.0);

    // 4. Per slot.
    const bool delayActive = flag(p[kPitchDrift]) && !flag(p[kZeroLat]);
    const int scMode = enumIdx(p[kScSource], 2);
    for (int s = 0; s < 2; ++s) {
        SlotState& st = slot_[s];
        core::SlotModulation& m = out.slot[s];
        m = base;
        if (frames > 0) {
            drift_[s].advance(frames);
            const double cutoffM = drift_[s].cutoffOct(d) + lastTempo_.cutoffOct;
            const auto domain = core::cutoffDomain(p[kFreqIdx[s]], fs_);
            const double pManual = referencePitch(domain.baseHz, a4);
            if (keyEnabled) {
                const uint16_t mask = scale::maskFor(enumIdx(p[kKeyScale], 5), uint16_t(std::clamp(num(p[kKeyMask], 4095.0), 0.0, 4095.0)));
                const int root = enumIdx(p[kKeyRoot], 11);
                st.quant.configure(mask, root);
                const double pEff = pManual + key_.weight() * (key_.notePitch() - pManual);
                const double target = admittedTargetPitch(pEff + 12.0 * cutoffM, domain, a4, mask, root);
                const double g = st.quant.update(target, dt);
                // A base/rate/tuning edit can move an already running glide
                // outside the new range. Safety bounds still apply during it.
                st.lastQuantOff = std::clamp((g - pManual) / 12.0, -core::kCreativeCutoffOctaves,
                                             std::log2(domain.maxHz / domain.baseHz));
            } else {
                st.quant.reset();
            }
            st.lockW = approach(st.lockW, keyEnabled ? 1.0 : 0.0, dt / 0.05);
            st.delayHold = approach(st.delayHold, keyEnabled ? 0.0 : 1.0, dt / 0.1);
            st.cutoffOct = cutoffM + st.lockW * (st.lastQuantOff - cutoffM);
            st.delayMs = drift_[s].delayMs(d) * st.delayHold;
        }
        // Harmonic targeting: pick up a newly published map at the block boundary.
        const HarmonicMap* map = st.pendingMap.load(std::memory_order_acquire);
        st.mapInUse.store(map, std::memory_order_release);
        st.harmonic.setMap(map);
        HarmonicLimits lim;
        lim.biasLo = map ? map->biasMin() : -1.0;
        lim.biasHi = map ? map->biasMax() : 1.0;
        const bool avail = st.harmonic.update(std::clamp(num(p[kHTarget]), -1.0, 1.0), unit(p[kHAmount]),
                                              num(p[kBiasIdx[s]]), num(p[kDriveIdx[s]]), dt, lim);
        status_.harmonicAvailable[s] = avail;

        m.creativeCutoffOct = st.cutoffOct;
        m.creativeDelayActive = delayActive;
        m.creativeDelayMs = delayActive ? st.delayMs : 0.0;
        m.recoveryTauS = lastTempo_.recoveryTauS;
        m.sidechainMode = scMode;
        m.driveOffsetDb += avail ? st.harmonic.driveDeltaDb() : 0.0;
        m.biasOffset += avail ? st.harmonic.biasDelta() : 0.0;
    }

    // 5. Per-sample buffers.
    const double* L = (mainIn.channels && mainIn.numChannels > 0) ? mainIn.channels[0] : nullptr;
    const double* R = (mainIn.channels && mainIn.numChannels > 1) ? mainIn.channels[1] : nullptr;
    transient_.process(L, R, frames, unit(p[kTransient]), transBuf_.data());
    const bool scConnected = scIn && scIn->channels && scIn->numChannels > 0 && scIn->channels[0];
    sidechain_.setBudgetAmps(budgetA_.load(std::memory_order_relaxed));
    sidechain_.process(SidechainSource(scMode), num(p[kScDepth]), num(p[kScHpf], 40.0),
                       scConnected ? scIn->channels[0] : nullptr,
                       (scConnected && scIn->numChannels > 1) ? scIn->channels[1] : nullptr, scConnected, frames,
                       loadBuf_.data());
    out.externalLoadA = loadBuf_.data();
    out.transientPreserve = transBuf_.data();

    // 6. Status.
    status_.tempoActive = ti.enabled;
    status_.tempoFallback = lastTempo_.fallback;
    status_.tempoPhase = lastTempo_.phase;
    status_.scaleLockActive = keyEnabled;
    status_.pitchDriftHeld = keyEnabled && delayActive;
    status_.keyState = keyEnabled ? key_.state() : KeyState::Off;
    {
        const double pm = referencePitch(core::cutoffDomain(p[kFreqIdx[0]], fs_).baseHz, a4);
        status_.referenceNote = pm + key_.weight() * (key_.notePitch() - pm);
    }
    status_.sidechainConnected = scConnected;
    status_.sidechainMissing = scMode != int(SidechainSource::Main) && !scConnected;
}

}  // namespace cd::control
