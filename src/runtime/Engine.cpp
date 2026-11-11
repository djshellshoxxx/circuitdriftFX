// CircuitDrift runtime: integrated engine implementation (see Engine.h). Owner: cd_runtime.
#include "runtime/Engine.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>

namespace cd::runtime {
namespace {

void bump(std::int64_t& c) noexcept { if (c < std::numeric_limits<std::int64_t>::max()) ++c; }

void silence(const AudioBuses& b, uint32_t frames) noexcept {
    if (!b.out || b.channels < 1 || b.channels > 64) return;
    for (int c = 0; c < b.channels; ++c)
        if (b.out[c]) std::fill(b.out[c], b.out[c] + frames, 0.0);
}

// Permanent host IDs (docs/planned-features/parameters.json; 1000-1999 never reassigned).
constexpr uint32_t kMacroAge = 1000, kMacroSag = 1001, kMacroDrift = 1002, kMacroWarmth = 1003, kAnalogNoise = 1004,
                   kInputGain = 1005, kOutputGain = 1006, kMix = 1007, kGainMatch = 1008, kSoftClip = 1009,
                   kHpfEnabled = 1010, kHpfFrequency = 1011, kSlotALifecycle = 1018, kSlotAScenario = 1019,
                   kSlotBLifecycle = 1020, kSlotBScenario = 1021, kPitchDrift = 1016, kScSource = 1022, kScDepth = 1023,
                   kScHpf = 1024, kTransient = 1025, kTempoEnabled = 1031, kTempoDivision = 1032, kTempoModifier = 1033,
                   kTempoDepth = 1034;

constexpr uint64_t kStateMagic = 0x3154535f454e4443ull;   // "CDNE_ST1"

struct StateHeader {
    uint64_t magic = kStateMagic;
    int64_t samples = 0;
    uint64_t epoch = 0;
    int64_t haveEpoch = 0, channels = 0, maxBlock = 0;
};

static_assert(std::is_trivially_copyable_v<MacroRuntime>, "MacroRuntime is part of the snapshot layout");

double clamp01(double x) noexcept { return std::isfinite(x) ? std::clamp(x, 0.0, 1.0) : 0.0; }

// Re-evaluates effective macro values from smoothed values v = {a,s,d,w,u}.
void evaluateMacros(MacroRuntime& m, const control::ManifestBindings& mb, const control::BaseValues& base, double fs,
                    const double* v, const control::ModulationInput* mod = nullptr, const control::FaultOverlay* fault = nullptr) noexcept {
    const blueprint::Condition cond = m.timeline.condition();
    control::TimelineOffsets tl;
    tl.serviceAgeYears = cond.values.ageYears;
    tl.tolSigma = cond.values.toleranceSigma;
    tl.leakFrac = cond.values.leakageFraction;
    tl.driveDb = 0.0;
    tl.biasOffset = cond.values.biasOffsetSpan * mb.biasSpan;
    control::Hooks hooks;
    hooks.timeline = &tl;
    hooks.mod = mod;
    hooks.fault = fault;
    m.eff = control::evaluate(base, control::MacroValues{v[0], v[1], v[2], v[3]}, mb, hooks);
    m.drive = m.eff.supportsWarmth ? std::pow(10.0, m.eff.driveDb / 20.0) : 1.0;
    if (!(m.drive > 0 && std::isfinite(m.drive))) m.drive = 1.0;
    m.shelf = m.eff.supportsAge ? control::designHighShelf(fs, m.eff.shelfHz, m.eff.shelfGainDb, m.eff.shelfQ) : control::BiquadCoeffs{};
    for (int k = 0; k < 5; ++k) m.last[k] = v[k];
}

void initMacros(MacroRuntime& m, const EngineConfig& cfg, const control::ManifestBindings& mb, double fs) noexcept {
    const double init[4] = {clamp01(cfg.initialMacros.age), clamp01(cfg.initialMacros.sag), clamp01(cfg.initialMacros.drift),
                            clamp01(cfg.initialMacros.warmth)};
    m.smoother.prepare(fs, init, 4, 0.020);
    for (int k = 0; k < 4; ++k) m.targets[k] = init[k];
    m.timeline.prepare(fs);
    m.timeline.setScenario(cfg.initialScenario);
    blueprint::TimelineSnapshot ts = m.timeline.snapshot();
    ts.target = ts.smoothed = clamp01(cfg.initialLifecycle);
    ts.rampLeft = 0; ts.rampStep = 0;
    m.timeline.restore(ts);
    const double v[5] = {init[0], init[1], init[2], init[3], m.timeline.smoothedU()};
    evaluateMacros(m, mb, cfg.base, fs, v);
    for (auto& z : m.z) z[0] = z[1] = 0.0;
}

void hashBindings(Fnv64& h, const control::ManifestBindings& b) noexcept {
    const bool flags[5] = {b.hasAge, b.hasSag, b.hasWarmth, b.hasDrift, b.analogNoise};
    h.bytes(flags, sizeof flags);
    const double d[7] = {b.cBase, b.cMin, b.biasSpan, b.leakRange, b.maxTolSigma, b.fluxMin, b.fluxMax};
    h.bytes(d, sizeof d);
}

// Fault outputs -> control::FaultOverlay (Macros "faults" stage). Leakage: log-position of the
// leakage resistance between its nominal and short-equivalent bounds, scaled by the manifest's
// declared safe leakage range; bias: perturbation as a fraction of the fault's bias span, in the
// manifest's bias-span units. No load-multiplier contribution is defined for these four faults.
control::FaultOverlay faultHook(const blueprint::FaultOutputs& o, const blueprint::FaultBounds& b,
                                const control::ManifestBindings& mb) noexcept {
    control::FaultOverlay h;
    if (o.leakageROhm > 0 && b.nominalLeakR > b.shortLeakR) {
        const double x = std::log(b.nominalLeakR / o.leakageROhm) / std::log(b.nominalLeakR / b.shortLeakR);
        if (std::isfinite(x)) h.leakFracAdd = std::clamp(x, 0.0, 1.0) * mb.leakRange;
    }
    if (b.biasSpanV > 0 && std::isfinite(o.biasPerturbationV)) h.biasOffsetAdd = o.biasPerturbationV / b.biasSpanV * mb.biasSpan;
    return h;
}

bool sameHook(const control::FaultOverlay& a, const control::FaultOverlay& b) noexcept {
    return a.leakFracAdd == b.leakFracAdd && a.loadMultiplierScale == b.loadMultiplierScale && a.biasOffsetAdd == b.biasOffsetAdd;
}

bool configureSidechain(OverlayRuntime& o, double fs) noexcept {
    control::SidechainLoadDetector::Config c;
    c.source = control::SidechainLoadDetector::Source::External;
    c.hpfHz = o.prm.sidechainHpfHz; c.depth = 1.0; c.iBudgetAmps = 1.0;   // returns clamp(E/E_ref,0,4); depth*I_budget applied by the engine
    return o.sidechain.prepare(fs, c);
}

double clampOr(double x, double lo, double hi, double dflt) noexcept { return std::isfinite(x) ? std::clamp(x, lo, hi) : dflt; }

void configureTempo(OverlayRuntime& o) noexcept {
    o.tempo.setDivision(control::Division(std::min<std::uint32_t>(o.prm.tempoDivision, 5)),
                        control::Modifier(std::min<std::uint32_t>(o.prm.tempoModifier, 2)));
}

}  // namespace

// Re-evaluates with the overlay hooks the prepared engine currently has: modulation only when Drift
// is bound (q = creative motion + tempo drift), fault hook only while a fault channel is not cleared.
// With no Drift binding and no active fault this is exactly the hook-less evaluation.
void Engine::evaluateWithOverlays(PreparedEngine& p, const double* v) noexcept {
    OverlayRuntime& o = p.overlay_;
    control::ModulationInput mod;
    mod.q = o.modQ;
    o.evalQ = o.modQ;
    evaluateMacros(p.macro_, p.bindings_, p.base_, p.fs_, v, p.bindings_.hasDrift ? &mod : nullptr, o.hookActive ? &o.hook : nullptr);
}

control::ManifestBindings syntheticMacroBindings() noexcept {
    control::ManifestBindings b;
    b.hasAge = true;      // creative HF shelf (not an ageing equation)
    b.hasWarmth = true;   // pre-stage drive dB only (no bias span / flux domain in the fixtures)
    return b;
}

bool Engine::resolveQuality(const QualitySettings& q, int override, dsp::Oversampler::Spec& spec, QualityInfo& info) noexcept {
    spec = dsp::Oversampler::Spec{};
    QualityInfo i;
    switch (q.mode) {
        case QualityMode::Eco: spec.factor = 4; spec.family = dsp::OsFamily::Fir; i.maxNewton = 6; break;
        case QualityMode::Studio: spec.factor = 4; spec.family = dsp::OsFamily::Fir; i.maxNewton = 8; break;
        case QualityMode::Render: spec.factor = 16; spec.family = dsp::OsFamily::Fir; i.maxNewton = 12; break;
        case QualityMode::Manual:
            if (q.manualFactor != 1 && q.manualFactor != 2 && q.manualFactor != 4 && q.manualFactor != 8 && q.manualFactor != 16)
                return false;
            if (q.family != dsp::OsFamily::Fir && q.family != dsp::OsFamily::Iir) return false;
            spec.factor = q.manualFactor; spec.family = q.family; i.maxNewton = 8;
            break;
        default: return false;
    }
    if (q.zeroLatency) { spec.factor = 1; spec.family = dsp::OsFamily::Fir; }
    if (override != 0) {
        if (override < 1 || override > 12) return false;
        i.maxNewton = override;
    }
    i.factor = spec.factor; i.family = spec.family; i.zeroLatency = q.zeroLatency;
    i.draft = spec.factor < 4;
    i.familyException = spec.family == dsp::OsFamily::Iir;
    info = i;
    return true;
}

std::unique_ptr<PreparedEngine> Engine::buildPrepared(const EngineConfig& cfg, std::string& err) {
    auto p = std::make_unique<PreparedEngine>();
    dsp::Oversampler::Spec spec;
    if (!resolveQuality(cfg.quality, cfg.maxNewtonOverride, spec, p->quality_)) { err = "invalid quality settings"; return nullptr; }
    CircuitConfig cc = cfg.circuit;
    cc.sampleRate = cfg.sampleRate; cc.maxBlock = cfg.maxBlock; cc.channels = cfg.channels;
    cc.os = spec; cc.maxNewton = p->quality_.maxNewton; cc.padToFamilyMax = !cfg.quality.zeroLatency;
    if (!p->circuit_.prepare(cc, err)) return nullptr;
    // D09 pitch-drift delay: only with pitchDrift.enabled, a Drift binding and not Zero Latency. Its
    // base delay (1.5 ms, rounded to whole samples; exact at 48/96/192 kHz, <= 0.5 sample residual
    // otherwise) is added to the declared latency and the dry path is delayed to match.
    const OverlayConfig& ocfg = cfg.overlays;
    p->pitchDriftParam_ = ocfg.pitchDriftEnabled;
    p->driftActive_ = ocfg.pitchDriftEnabled && cfg.macroBindings.hasDrift && !cfg.quality.zeroLatency;
    int driftLat = 0;
    if (p->driftActive_) {
        driftLat = int(std::lround(control::PitchDriftDelay::kBaseMs * 1e-3 * cfg.sampleRate));
        for (int c = 0; c < cfg.channels; ++c) {
            if (!p->pitch_[c].prepare(cfg.sampleRate)) { err = "pitch-drift delay rejected sample rate"; return nullptr; }
            p->pitch_[c].setEnabled(true);
        }
        p->pitchState_.assign(p->pitch_[0].stateSize(), 0.0);
    }
    for (auto& d : p->driftDry_) d.prepare(driftLat);
    OutputConfig oc;
    oc.sampleRate = cfg.sampleRate; oc.maxBlock = cfg.maxBlock; oc.channels = cfg.channels;
    oc.clipOs = spec; oc.padToFamilyMax = !cfg.quality.zeroLatency;
    oc.upstreamLatency = p->circuit_.latencySamples() + driftLat; oc.initial = cfg.initial;
    if (!p->output_.prepare(oc, err)) return nullptr;
    p->quality_.circuitLatency = p->circuit_.latencySamples();
    p->quality_.clipLatency = p->output_.latencySamples();
    p->quality_.latencySamples = p->output_.totalLatencySamples();
    p->quality_.driftLatency = driftLat;
    p->os_ = spec; p->pad_ = !cfg.quality.zeroLatency;
    p->fs_ = cfg.sampleRate; p->channels_ = cfg.channels; p->maxBlock_ = cfg.maxBlock;
    const std::size_t sz = std::size_t(cfg.channels) * std::size_t(cfg.maxBlock);
    p->raw_.assign(sz, 0.0); p->gained_.assign(sz, 0.0); p->wet_.assign(sz, 0.0); p->dry_.assign(sz, 0.0);
    p->wet2_.assign(sz, 0.0); p->dry2_.assign(sz, 0.0);
    p->drive_.assign(std::size_t(cfg.maxBlock), 1.0);
    p->shelf_.assign(std::size_t(cfg.maxBlock) * 5, 0.0);
    p->driftD_.assign(std::size_t(cfg.maxBlock), 0.0);
    p->driftQ_.assign(std::size_t(cfg.maxBlock), 0.0);
    p->bindings_ = cfg.macroBindings;
    p->base_ = cfg.base;
    p->factor_ = spec.factor;
    initMacros(p->macro_, cfg, p->bindings_, cfg.sampleRate);
    // ---- overlays: detectors, tempo, creative motion, faults ----
    OverlayRuntime& o = p->overlay_;
    o = OverlayRuntime{};
    o.prm.transientAmount = clampOr(ocfg.transientAmount, 0, 1, 0);
    o.prm.sidechainDepth = clampOr(ocfg.sidechainDepth, 0, 2, 0);
    o.prm.sidechainHpfHz = clampOr(ocfg.sidechainHpfHz, 20, 300, 40);
    if (!(std::isfinite(ocfg.sidechainBudgetAmps) && ocfg.sidechainBudgetAmps >= 0)) { err = "sidechain I_budget must be finite amperes >= 0"; return nullptr; }
    o.prm.sidechainBudgetAmps = ocfg.sidechainBudgetAmps;
    o.prm.sidechainSource = std::uint32_t(ocfg.sidechainSource) <= 2 ? std::uint32_t(ocfg.sidechainSource) : 0;
    o.prm.tempoEnabled = ocfg.tempoEnabled ? 1 : 0;
    o.prm.tempoDivision = std::min<std::uint32_t>(std::uint32_t(ocfg.tempoDivision), 5);
    o.prm.tempoModifier = std::min<std::uint32_t>(std::uint32_t(ocfg.tempoModifier), 2);
    o.prm.tempoDepth = clampOr(ocfg.tempoDepth, 0, 1, 0);
    if (!o.transient.prepare(cfg.sampleRate) || !configureSidechain(o, cfg.sampleRate) || !o.tempo.prepare(cfg.sampleRate)) {
        err = "detector/tempo preparation rejected sample rate";
        return nullptr;
    }
    configureTempo(o);
    const std::uint32_t slot = cfg.slotRole == core::ParamScope::SlotB ? 1u : 0u;
    o.motion.prepare(cfg.sampleRate, control::deriveSeed(ocfg.motionSeed, slot, "cd.runtime.engine.drift", control::StreamType::DelayMotion));
    if (p->bindings_.hasDrift) o.modQ = std::clamp(o.motion.q(), -1.0, 1.0);   // tempo drift is 0 at phase 0
    const double rate = cfg.sampleRate * double(spec.factor);
    int applied = 0;
    for (int k = 0; k < kMaxFaultSlots; ++k) {
        const FaultSlotConfig& f = ocfg.faults[std::size_t(k)];
        p->faultAvail_[k] = false;
        p->faultUsed_[k] = f.used;
        p->faultReason_[k] = f.used ? "" : "slot unused";
        p->faultPartial_[k] = "";
        p->faultElement_[k] = f.used ? f.element : -1;
        p->faultParam_[k] = -1;
        p->faultNominal_[k] = 0;
        if (!f.used) continue;
        const auto& els = cfg.circuit.netlist.elements;
        if (f.element < 0 || f.element >= int(els.size()) || els[std::size_t(f.element)].kind != dsp::NetElement::Kind::Device ||
            !els[std::size_t(f.element)].model) {
            err = "fault slot must name a device element of the circuit netlist";
            return nullptr;
        }
        const dsp::DeviceManifest& man = *els[std::size_t(f.element)].model;
        const auto eq = man.equation;
        const char* why = "";
        if (!blueprint::faultAvailable(f.overlay.type, eq == dsp::DeviceEquation::Capacitor, eq == dsp::DeviceEquation::Resistor,
                                       false /* no tube equation family exists */, why)) {
            p->faultReason_[k] = why;   // unavailable with a reason; Apply events for it are rejected
            continue;
        }
        // Parameter action target and manifest-derived bounds (synthetic placeholders: the fixture
        // manifests declare no open/short equivalents, so the manifest range limits stand in).
        blueprint::FaultBounds b = f.bounds;
        int pk = -1;
        double nom = 0, lo = 0, hi = 0;
        switch (f.overlay.type) {
            case blueprint::FaultType::CrackedResistor:
                pk = man.paramIndex("resistanceOhm");
                if (pk < 0 || !p->circuit_.parameterRange(f.element, pk, nom, lo, hi)) {
                    p->faultReason_[k] = "resistor manifest exposes no resistanceOhm parameter";
                    pk = -1;
                    break;
                }
                if (!(nom < hi)) {
                    p->faultReason_[k] = "resistor nominal is at the manifest maximum: no open-equivalent headroom";
                    pk = -1;
                    break;
                }
                b.nominalR = nom;
                b.openR = hi;           // placeholder open equivalent: manifest max (never ideal infinity)
                break;
            case blueprint::FaultType::ElectrolyticOpen:
                pk = man.paramIndex("capacitanceF");
                if (pk < 0 || !p->circuit_.parameterRange(f.element, pk, nom, lo, hi)) {
                    p->faultReason_[k] = "capacitor manifest exposes no capacitanceF parameter";
                    pk = -1;
                    break;
                }
                if (!(nom > lo)) {
                    p->faultReason_[k] = "capacitor nominal is at the manifest minimum: no open-equivalent headroom";
                    pk = -1;
                    break;
                }
                b.nominalC = nom;
                b.minC = lo;            // manifest minimum capacitance (never ideal zero)
                p->faultPartial_[k] = "series-resistance rise unavailable: the capacitor manifest has no ESR parameter or series node";
                break;
            case blueprint::FaultType::ElectrolyticLeaky:
                p->faultReason_[k] = "no leakage-resistance parameter or parallel-leakage topology in the capacitor manifest";
                break;
            case blueprint::FaultType::MicrophonicTube:
                p->faultReason_[k] = "no validated tube topology or model is available (G01 open)";
                break;
        }
        if (pk < 0) continue;
        std::string fe;
        if (!o.faults[k].prepare(rate, b, f.overlay, fe)) { err = "fault slot: " + fe; return nullptr; }
        p->faultAvail_[k] = true;
        p->faultBounds_[k] = b;
        p->faultParam_[k] = pk;
        p->faultNominal_[k] = nom;
        o.prm.faults[k] = f.overlay;
        ++applied;
    }
    // Per-substep schedules for the built-in circuit's parameter tracks.
    p->faultSched_.assign(std::size_t(applied) * std::size_t(cfg.maxBlock) * std::size_t(spec.factor), 0.0);
    p->trackCount_ = 0;
    for (int k = 0, j = 0; k < kMaxFaultSlots; ++k) {
        p->faultTrack_[k] = nullptr;
        if (!p->faultAvail_[k]) continue;
        p->faultTrack_[k] = p->faultSched_.data() + std::size_t(j++) * std::size_t(cfg.maxBlock) * std::size_t(spec.factor);
        p->tracks_[p->trackCount_++] = {p->faultElement_[k], p->faultParam_[k], p->faultTrack_[k]};
    }
    {
        const double v[5] = {p->macro_.last[0], p->macro_.last[1], p->macro_.last[2], p->macro_.last[3], p->macro_.last[4]};
        evaluateWithOverlays(*p, v);
    }
    p->overlayInit_ = o;
    p->fadeLen_ = int(std::max(1.0, std::round(kGraphFadeSeconds * cfg.sampleRate)));
    Fnv64 h;
    h.str("cd.runtime.Engine/1");
    h.bytes(state::kEngineModelVersion.data(), state::kEngineModelVersion.size());
    h.pod(cfg.channels); h.pod(p->quality_.factor); h.pod(p->quality_.family); h.pod(p->quality_.maxNewton);
    h.pod(p->quality_.zeroLatency); h.pod(p->quality_.latencySamples);
    hashBindings(h, p->bindings_);
    h.pod(p->driftActive_); h.pod(driftLat);
    for (int k = 0; k < kMaxFaultSlots; ++k) { h.pod(p->faultAvail_[k]); h.pod(ocfg.faults[std::size_t(k)].overlay.type); }
    p->configHash_ = h.h;
    p->generation_ = ++nextGeneration_;
    err.clear();
    return p;
}

bool Engine::tryInstallPrepared(std::unique_ptr<PreparedEngine>& h) noexcept {
    if (!h) return false;
    if (active_ && h->generation_ <= active_->generation_) { bump(staleInstalls_); return false; }
    active_.swap(h);  // previous engine (or null) now in h: caller reclaims off-audio
    if (fading_) {    // a running fade may reference the previous engine's built-in circuit
        fading_ = false; oldGraph_ = nullptr; fadePos_ = 0;
        if (graphs_ && curGraph_) graphs_->releasedBelow_.store(curGraph_->generation(), std::memory_order_release);
    }
    if (curGraph_) {
        const GraphTarget t = graphTarget(), g = curGraph_->target();
        if (t.sampleRate != g.sampleRate || t.channels != g.channels || t.maxBlock != g.maxBlock || t.os.factor != g.os.factor ||
            t.os.family != g.os.family || t.maxNewton != g.maxNewton || t.circuitLatency != g.circuitLatency)
            dropGraphs();
    }
    return true;
}

bool Engine::prepare(const EngineConfig& cfg, std::string& err) {
    auto p = buildPrepared(cfg, err);
    if (!p) return false;
    if (!tryInstallPrepared(p)) { err = "stale generation"; return false; }
    p.reset();  // reclaim previous engine here (non-audio caller)
    role_ = cfg.slotRole;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Events

void Engine::applyEvent(const ParamEvent& e) noexcept {
    PreparedEngine& p = *active_;
    OutputStage& o = p.output_;
    OverlayRuntime& ov = p.overlay_;
    const bool on = e.value >= 0.5;
    switch (e.target) {
        case ParamTarget::InputGainDb: o.setInputGainDb(e.value); break;
        case ParamTarget::OutputTrimDb: o.setOutputTrimDb(e.value); break;
        case ParamTarget::Mix: o.setMix(e.value); break;
        case ParamTarget::AutoGain: o.setAutoGain(on); break;
        case ParamTarget::HpfEnabled: o.setHpfEnabled(on); break;
        case ParamTarget::HpfCutoffHz: o.setHpfCutoffHz(e.value); break;
        case ParamTarget::SoftClipEnabled: o.setSoftClipEnabled(on); break;
        case ParamTarget::HostBypass: o.setHostBypass(on); break;
        case ParamTarget::FaultEnabled:
        case ParamTarget::FaultSeverity: {
            if (e.index >= std::uint32_t(kMaxFaultSlots) || !p.faultAvail_[e.index] || !std::isfinite(e.value)) { bump(ov.faultRejected); break; }
            blueprint::FaultChannel& ch = ov.faults[e.index];
            blueprint::FaultOverlay f = ov.prm.faults[e.index];
            if (e.target == ParamTarget::FaultEnabled) f.enabled = on;
            else f.severity = std::clamp(e.value, 0.0, 1.0);
            f.revision = std::max(f.revision, ch.overlay().revision) + 1;
            if (ch.apply(f)) { ov.prm.faults[e.index] = f; bump(ov.faultApplied); }
            else bump(ov.faultRejected);
            break;
        }
        case ParamTarget::FaultResetAll:
            for (int k = 0; k < kMaxFaultSlots; ++k) {
                if (!p.faultAvail_[k]) continue;
                ov.faults[k].reset();
                blueprint::FaultOverlay& f = ov.prm.faults[k];
                f.enabled = false; f.severity = 0; f.revision = std::max(f.revision, ov.faults[k].overlay().revision) + 1;
            }
            bump(ov.faultResets);
            break;
        default: break;
    }
}

void Engine::applyHostParam(PreparedEngine& p, uint32_t hostId, double normalized) noexcept {
    const core::ParamSpec* spec = core::findByHostId(hostId);
    if (!spec) { bump(unknown_); return; }
    if (spec->scope != core::ParamScope::Global && spec->scope != role_) { bump(otherSlot_); return; }
    bump(paramEvents_);
    const double v = core::toPlain(*spec, normalized);
    const bool on = v >= 0.5;
    OutputStage& o = p.output_;
    MacroRuntime& m = p.macro_;
    OverlayRuntime& ov = p.overlay_;
    switch (hostId) {
        case kMacroAge: case kMacroSag: case kMacroDrift: case kMacroWarmth:
            m.targets[hostId - kMacroAge] = clamp01(v);
            m.smoother.setTargets(m.targets, 4);
            if ((hostId == kMacroSag && !p.bindings_.hasSag) || (hostId == kMacroDrift && !p.bindings_.hasDrift))
                bump(unsupported_);   // stored, neutral: no binding in this circuit
            break;
        case kAnalogNoise: m.analogNoise = on ? 1 : 0; bump(unsupported_); break;   // no noise generator
        case kInputGain: o.setInputGainDb(v); break;
        case kOutputGain: o.setOutputTrimDb(v); break;
        case kMix: o.setMix(v); break;
        case kGainMatch: o.setAutoGain(on); break;
        case kSoftClip: o.setSoftClipEnabled(on); break;
        case kHpfEnabled: o.setHpfEnabled(on); break;
        case kHpfFrequency: o.setHpfCutoffHz(v); break;
        case kSlotALifecycle: case kSlotBLifecycle: m.timeline.setTarget(v); break;
        case kSlotAScenario: case kSlotBScenario: m.timeline.setScenario(blueprint::Scenario(int(v))); break;
        // Enabling/disabling the pitch-drift delay changes latency: prepare-time only (latency handshake).
        case kPitchDrift: if (on != p.pitchDriftParam_) bump(unsupported_); break;
        case kScSource: ov.prm.sidechainSource = std::uint32_t(std::clamp(int(std::lround(v)), 0, 2)); break;
        case kScDepth: ov.prm.sidechainDepth = clampOr(v, 0, 2, ov.prm.sidechainDepth); break;
        case kScHpf: {   // re-designing the HPF restarts the sidechain detector's filter/envelope state
            const double hz = clampOr(v, 20, 300, ov.prm.sidechainHpfHz);
            if (hz != ov.prm.sidechainHpfHz) { ov.prm.sidechainHpfHz = hz; configureSidechain(ov, p.fs_); }
            break;
        }
        case kTransient: ov.prm.transientAmount = clampOr(v, 0, 1, ov.prm.transientAmount); break;
        case kTempoEnabled: ov.prm.tempoEnabled = on ? 1 : 0; break;
        case kTempoDivision: ov.prm.tempoDivision = std::uint32_t(std::clamp(int(std::lround(v)), 0, 5)); configureTempo(ov); break;
        case kTempoModifier: ov.prm.tempoModifier = std::uint32_t(std::clamp(int(std::lround(v)), 0, 2)); configureTempo(ov); break;
        case kTempoDepth: ov.prm.tempoDepth = clampOr(v, 0, 1, ov.prm.tempoDepth); break;
        default: bump(unsupported_); break;
    }
}

void Engine::applyTransaction(PreparedEngine& p, const state::TransactionEvent& e) noexcept {
    switch (e.kind) {
        case state::EventKind::Param: applyHostParam(p, e.hostId, e.normalized); break;
        case state::EventKind::ConfigPublish: bump(configEvents_); break;
        case state::EventKind::NoteOff:
        case state::EventKind::AllNotesOff: bump(noteEvents_); break;
        default: bump(unknown_); break;
    }
}

// ---------------------------------------------------------------------------------------------
// Processing

CircuitRuntime& Engine::currentCircuit(PreparedEngine& p) noexcept { return curGraph_ ? curGraph_->circuit() : p.circuit_; }
const CircuitRuntime& Engine::currentCircuit() const noexcept { return curGraph_ ? curGraph_->circuit() : active_->circuit_; }

// Per host sample, in the spec order restricted to what exists: macro smoother / lifecycle /
// creative-motion and tempo targets -> transient + sidechain detectors -> fault channels at the
// electrical rate (excited by the main transient detector) -> effective-value evaluation (base ->
// timeline -> macros -> modulation q -> fault hook -> clamp) whenever an input changed.
void Engine::macroSlice(PreparedEngine& p, const double* const* gained, const AudioBuses& buses, int off, int n, uint32_t& st) noexcept {
    MacroRuntime& m = p.macro_;
    OverlayRuntime& o = p.overlay_;
    const std::size_t M = std::size_t(p.maxBlock_);
    double* b0 = p.shelf_.data(); double* b1 = b0 + M; double* b2 = b1 + M; double* a1 = b2 + M; double* a2 = a1 + M;
    const int C = p.channels_;
    const bool drift = p.bindings_.hasDrift;
    const bool tempoOn = o.prm.tempoEnabled != 0;
    const auto src = control::SidechainSource(o.prm.sidechainSource);
    const bool scActive = scConnected_ && src != control::SidechainSource::Main;
    o.sidechain.setConnected(scActive);   // inactive: the detector returns 0 and holds no stale energy
    const double* sc[2] = {nullptr, nullptr};
    if (scActive) {
        sc[0] = buses.sidechain[0] + off;
        sc[1] = buses.sidechainChannels > 1 ? buses.sidechain[1] + off : sc[0];
    }
    const double scScale = o.prm.sidechainDepth * o.prm.sidechainBudgetAmps;
    int slots[kMaxFaultSlots];
    int nf = 0;
    for (int k = 0; k < kMaxFaultSlots; ++k) if (p.faultAvail_[k]) slots[nf++] = k;
    const int F = p.factor_;
    for (int i = 0; i < n; ++i) {
        m.smoother.step();
        const double u = m.timeline.tick();
        o.tempo.step();
        if (drift) {
            o.motion.advanceSample();
            o.tempoDrift = tempoOn ? o.tempo.drift(o.prm.tempoDepth) : 0.0;
            o.modQ = std::clamp(o.motion.q() + o.tempoDrift, -1.0, 1.0);
        }
        // Detectors: control scalars only, never audio.
        const double l = gained[0][i], r = C > 1 ? gained[1][i] : l;
        o.b = o.transient.process(l, r);
        o.P = control::TransientDetector::strength(o.prm.transientAmount, o.b);
        const double ratio = scActive ? o.sidechain.process(sc[0][i], sc[1][i]) : o.sidechain.process(0.0, 0.0);
        o.iExt = scScale * ratio;
        // No supply port exists in the synthetic fixtures: I_phys = I_dyn = 0 (G01).
        o.iLoad = control::composeLoadCurrent(src, 0.0, 0.0, m.smoother.value(1), o.iExt);
        bool hookChanged = false;
        if (nf > 0) {
            control::FaultOverlay h;
            std::int64_t act = 0;
            for (int j = 0; j < nf; ++j) {
                const int k = slots[j];
                blueprint::FaultChannel& fc = o.faults[k];
                double* sched = p.faultTrack_[k] + std::size_t(i) * std::size_t(F);
                const bool res = o.prm.faults[k].type == blueprint::FaultType::CrackedResistor;
                for (int sub = 0; sub < F; ++sub) {
                    o.faultOut[k] = fc.tick(o.b);
                    sched[sub] = res ? o.faultOut[k].resistanceOhm : o.faultOut[k].capacitanceF;
                }
                const control::FaultOverlay hk = faultHook(o.faultOut[k], p.faultBounds_[k], p.bindings_);
                if (!fc.cleared() || hk.leakFracAdd != 0.0 || hk.biasOffsetAdd != 0.0) {
                    act = 1;
                    h.leakFracAdd += hk.leakFracAdd;
                    h.biasOffsetAdd += hk.biasOffsetAdd;
                }
            }
            if (act != o.hookActive || (act && !sameHook(h, o.hook))) {
                hookChanged = true;
                o.hookActive = act;
                o.hook = act ? h : control::FaultOverlay{};
            }
        }
        const double v[5] = {m.smoother.value(0), m.smoother.value(1), m.smoother.value(2), m.smoother.value(3), u};
        if (v[0] != m.last[0] || v[1] != m.last[1] || v[2] != m.last[2] || v[3] != m.last[3] || v[4] != m.last[4] || hookChanged ||
            (drift && o.modQ != o.evalQ))
            evaluateWithOverlays(p, v);
        p.drive_[std::size_t(i)] = m.drive;
        b0[i] = m.shelf.b0; b1[i] = m.shelf.b1; b2[i] = m.shelf.b2; a1[i] = m.shelf.a1; a2[i] = m.shelf.a2;
        if (p.driftActive_) { p.driftD_[std::size_t(i)] = v[2]; p.driftQ_[std::size_t(i)] = o.modQ; }
    }
    if (o.hookActive) st |= status::kFaultActive;
}

void Engine::runSlice(PreparedEngine& p, const AudioBuses& buses, int off, int n, uint32_t& st) noexcept {
    const double* const* in = buses.in;
    double* const* out = buses.out;
    const int C = p.channels_;
    const std::size_t M = std::size_t(p.maxBlock_);
    const double* rawC[2] = {};
    double* raw[2] = {}; double* gained[2] = {}; double* wet[2] = {}; double* dry[2] = {}; double* o[2] = {};
    double* wet2[2] = {}; double* dry2[2] = {};
    const double *gainedC[2] = {}, *wetC[2] = {}, *dryC[2] = {};
    for (int c = 0; c < C; ++c) {
        const std::size_t base = std::size_t(c) * M;
        raw[c] = p.raw_.data() + base; gained[c] = p.gained_.data() + base;
        wet[c] = p.wet_.data() + base; dry[c] = p.dry_.data() + base;
        wet2[c] = p.wet2_.data() + base; dry2[c] = p.dry2_.data() + base;
        rawC[c] = raw[c]; gainedC[c] = gained[c]; wetC[c] = wet[c]; dryC[c] = dry[c];
        o[c] = out[c] + off;
        const double* x = in[c] + off;
        for (int i = 0; i < n; ++i) {
            double v = x[i];
            if (!isFiniteSample(v)) { v = 0.0; bump(inputNonFinite_); st |= status::kInputSanitized; }
            else if (std::fabs(v) > kEntryLimit) { v = std::copysign(kEntryLimit, v); bump(inputClamped_); st |= status::kInputSanitized; }
            raw[c][i] = v;
        }
    }
    p.output_.applyInputGain(rawC, gained, n);
    macroSlice(p, gainedC, buses, off, n, st);
    const double* drive = p.drive_.data();
    CircuitRuntime& cur = currentCircuit(p);
    // Fault parameter tracks reach the built-in circuit only (graph paths map elements differently).
    auto tracks = [&](const CircuitRuntime& c) { return &c == &p.circuit_ ? p.tracks_ : nullptr; };
    auto ntracks = [&](const CircuitRuntime& c) { return &c == &p.circuit_ ? p.trackCount_ : 0; };
    if (!fading_) {
        cur.process(gainedC, wet, dry, n, drive, tracks(cur), ntracks(cur));
    } else {
        CircuitRuntime& old = oldGraph_ ? oldGraph_->circuit() : p.circuit_;
        old.process(gainedC, wet2, dry2, n, drive, tracks(old), ntracks(old));
        cur.process(gainedC, wet, dry, n, drive, tracks(cur), ntracks(cur));
        const double invLen = 1.0 / double(p.fadeLen_);
        for (int i = 0; i < n && fadePos_ < p.fadeLen_; ++i) {
            ++fadePos_;
            const double x = fadePos_ == p.fadeLen_ ? 1.0 : double(fadePos_) * invLen;
            for (int c = 0; c < C; ++c) {
                wet[c][i] = (1.0 - x) * wet2[c][i] + x * wet[c][i];   // linear equal-sum crossfade
                dry[c][i] = dry2[c][i];
            }
        }
        if (fadePos_ >= p.fadeLen_) {
            fading_ = false;
            oldGraph_ = nullptr;
            if (graphs_ && curGraph_) graphs_->releasedBelow_.store(curGraph_->generation(), std::memory_order_release);
        }
    }
    // Age: creative HF shelf on the wet path (TDF-II, per-sample coefficients).
    if (p.bindings_.hasAge) {
        const double* b0 = p.shelf_.data(); const double* b1 = b0 + M; const double* b2 = b1 + M;
        const double* a1 = b2 + M; const double* a2 = a1 + M;
        for (int c = 0; c < C; ++c) {
            double z0 = p.macro_.z[c][0], z1 = p.macro_.z[c][1];
            double* w = wet[c];
            for (int i = 0; i < n; ++i) {
                const double x = w[i];
                const double y = b0[i] * x + z0;
                z0 = b1[i] * x - a1[i] * y + z1;
                z1 = b2[i] * x - a2[i] * y;
                w[i] = y;
            }
            if (!isFiniteSample(z0) || !isFiniteSample(z1)) { z0 = z1 = 0.0; }
            p.macro_.z[c][0] = z0; p.macro_.z[c][1] = z1;
        }
    }
    // D09 creative pitch drift on the wet path (labelled creative effect, not a sag consequence);
    // the dry path is delayed by the integer base delay so the blend stays aligned.
    if (p.driftActive_) {
        const double* d = p.driftD_.data();
        const double* q = p.driftQ_.data();
        for (int c = 0; c < C; ++c) {
            double* w = wet[c];
            double* y = dry[c];
            for (int i = 0; i < n; ++i) {
                w[i] = p.pitch_[c].process(w[i], d[i], q[i]);
                y[i] = p.driftDry_[c].process(y[i]);
            }
        }
    }
    p.output_.process(rawC, dryC, wetC, o, n);
    samples_ += n;
}

void Engine::graphBoundary(PreparedEngine& p, uint32_t& st) noexcept {
    if (!graphs_ || fading_) return;
    auto& pool = graphs_->pool_;
    const uint64_t before = pool.installedGenerationAudio();
    const GraphInstaller::Holder* h = pool.installLatest();
    if (pool.installedGenerationAudio() == before) return;
    GraphProcessor* g = h ? h->proc.get() : nullptr;
    const GraphTarget t = graphTarget();
    if (!g || g->target().sampleRate != t.sampleRate || g->target().channels != t.channels || g->target().maxBlock != t.maxBlock ||
        g->target().os.factor != t.os.factor || g->target().os.family != t.os.family || g->target().maxNewton != t.maxNewton ||
        g->circuit().latencySamples() != p.quality_.circuitLatency || p.quality_.circuitLatency > p.fadeLen_) {
        bump(graphRejected_);
        return;
    }
    oldGraph_ = curGraph_;
    curGraph_ = g;
    fading_ = true;
    fadePos_ = 0;
    bump(graphInstalls_);
    st |= status::kGraphInstalled;
    if (!oldGraph_) graphs_->releasedBelow_.store(g->generation(), std::memory_order_release);
}

void Engine::captureBoundary() noexcept {
    if (!snap_ || !active_ || !snap_->captureRequested()) return;
    if (fading_) { bump(snapDeferred_); return; }
    const state::SnapshotIdentity id = snapshotIdentity();
    if (!(id == snap_->identity())) { bump(snapRefused_); return; }
    const std::span<std::byte> pl = snap_->writePayload();
    StateWriter w(pl.data(), pl.size());
    writeState(w);
    if (!w.ok() || w.size() != pl.size()) { bump(snapRefused_); return; }
    snap_->publish(uint64_t(samples_), currentCircuit().diagnostics().thermalPhase, curGraph_ ? curGraph_->generation() : 0);
    bump(snapPublished_);
}

bool Engine::validateCall(const ProcessContext& ctx, const AudioBuses& buses, ProcessResult& r) noexcept {
    PreparedEngine* p = active_.get();
    if (!p) { silence(buses, ctx.frames); r.status = status::kNotPrepared; return false; }
    if (ctx.sampleRate != p->fs_) { silence(buses, ctx.frames); r.status = status::kRateMismatch; return false; }
    bool layoutOk = buses.channels == p->channels_;
    if (layoutOk && ctx.frames > 0) {
        layoutOk = buses.in && buses.out;
        for (int c = 0; layoutOk && c < p->channels_; ++c) layoutOk = buses.in[c] && buses.out[c];
    }
    if (!layoutOk) { silence(buses, ctx.frames); r.status = status::kLayoutMismatch; return false; }
    if (haveEpoch_ && ctx.epoch != epoch_) { bump(epochChanges_); pendingSeek_ = true; }
    epoch_ = ctx.epoch; haveEpoch_ = true;
    return true;
}

// Per call: host transport -> TempoClock (a new epoch = transport seek clears the tempo phase lock
// and coefficient history, never capacitor/thermal/detector state; no pitch-detector history is
// wired yet) and the sidechain bus connection.
void Engine::callBoundary(PreparedEngine& p, const ProcessContext& ctx, const AudioBuses& b, uint32_t& st) noexcept {
    OverlayRuntime& o = p.overlay_;
    const bool seek = pendingSeek_;
    pendingSeek_ = false;
    if (seek) {
        o.tempo.prepare(p.fs_);
        configureTempo(o);
        bump(o.tempoResets);
    }
    control::HostTempo h;
    h.bpm = ctx.bpm; h.bpmValid = ctx.hasTempo;
    h.ppq = ctx.ppqPosition; h.ppqValid = ctx.hasPpq && ctx.playing;
    h.transportJump = seek;
    o.tempo.setHost(h);
    bool conn = b.sidechain != nullptr && (b.sidechainChannels == 1 || b.sidechainChannels == 2);
    for (int c = 0; conn && c < b.sidechainChannels; ++c) conn = b.sidechain[c] != nullptr;
    scConnected_ = conn;
    if (!conn && o.prm.sidechainSource != std::uint32_t(control::SidechainSource::Main)) st |= status::kSidechainDisconnected;
    if (o.prm.tempoEnabled && (o.tempo.status().bpmFallback || o.tempo.status().ppqFreeRun)) st |= status::kTempoFallback;
}

uint32_t Engine::sliceLimit(uint32_t pos, uint32_t end) const noexcept {
    if (!telemetry_) return end;
    const auto toNext = uint32_t(kTelemetryQuantum - uint64_t(samples_) % kTelemetryQuantum);
    return std::min(end, pos + toNext);
}

// Telemetry frame (CD-F17), built on the stack and handed to the wait-free writer (drop on full).
// Components: the dissipative branches mapped to thermal nodes, per channel (id = ch<<16 | element):
// thermal-node temperature [K], instantaneous branch power [W], current [A] and voltage [V] at the
// last solved substep. xy: input-source volts vs output-node volts per channel. No supply rail exists
// in the synthetic fixtures, so kValidRail stays clear. Values are synthetic-fixture volts/amps.
void Engine::publishTelemetry(PreparedEngine& p) noexcept {
    const CircuitRuntime& cr = currentCircuit(p);
    analysis::TelemetryFrame f;
    f.graphGeneration = std::uint32_t(curGraph_ ? curGraph_->generation() : 0);
    f.epoch = std::uint32_t(epoch_);
    f.sampleIndex = samples_;
    f.validityFlags = analysis::kValidXy | analysis::kValidSolver;
    const auto cd = cr.diagnostics();
    const auto od = p.output_.diagnostics();
    if (cd.fallbackLatched) f.fallbackFlags |= analysis::kFallbackSolverDiverged;
    if (od.bypassWeight > 0) f.fallbackFlags |= analysis::kFallbackBypassed;
    if (od.fallbackLatched) f.fallbackFlags |= analysis::kFallbackLimiterActive;
    f.solverIterations = std::uint32_t(std::max(0, cr.lastIterations(0)));
    f.solverResidual = float(cr.lastResidual(0));
    std::uint32_t n = 0;
    const int nb = cr.telemetryBranches();
    for (int c = 0; c < p.channels_; ++c) {
        for (int k = 0; k < nb && n < std::uint32_t(analysis::kMaxTelemetryComponents); ++k) {
            CircuitRuntime::BranchTelemetry bt;
            if (!cr.branchTelemetry(c, k, bt)) continue;
            analysis::ComponentTelemetry& t = f.components[n++];
            t.componentId = (std::uint32_t(c) << 16) | std::uint32_t(bt.element);
            t.flags = 1;
            t.temperatureK = float(bt.temperatureK);
            t.dissipationW = float(bt.powerW);
            t.currentA = float(bt.currentA);
            t.voltageV = float(bt.voltageV);
        }
        if (c < analysis::kMaxTelemetryXy) {
            f.xy[c].x = float(cr.inputVolts(c));
            f.xy[c].y = float(cr.outputVolts(c));
        }
    }
    f.componentCount = n;
    f.xyCount = std::uint32_t(p.channels_);
    f.xyKind = std::uint32_t(analysis::XyKind::InputOutputVolts);
    bump(telemetryFrames_);
    telemetry_->publish(f);
}

uint32_t Engine::finishStatus(PreparedEngine& p, uint32_t st) noexcept {
    const auto cd = currentCircuit(p).diagnostics();
    const auto od = p.output_.diagnostics();
    if (cd.fallbackLatched) st |= status::kCircuitFallback;
    if (cd.resetRequested) st |= status::kResetRequested;
    if (od.fallbackLatched) st |= status::kContainment;
    if (fading_) st |= status::kGraphFading;
    return st;
}

ProcessResult Engine::process(const ProcessBlock& b) noexcept {
    ProcessResult r;
    if (!validateCall(b.ctx, b.buses, r)) return r;
    PreparedEngine* p = active_.get();
    uint32_t st = 0;
    graphBoundary(*p, st);
    callBoundary(*p, b.ctx, b.buses, st);
    const uint32_t frames = b.ctx.frames;
    const uint32_t total = b.events ? b.eventCount : 0;
    const uint32_t count = std::min(total, kMaxEventsPerCall);
    uint32_t pos = 0, ev = 0;
    for (;;) {
        // Events in bounded sample order: an offset behind the current position applies now.
        while (ev < count && std::min(b.events[ev].offset, frames) <= pos) applyEvent(b.events[ev++]);
        if (pos >= frames) break;
        uint32_t end = frames;
        if (ev < count) end = std::min(end, std::min(b.events[ev].offset, frames));
        end = sliceLimit(pos, std::min(end, pos + uint32_t(p->maxBlock_)));
        runSlice(*p, b.buses, int(pos), int(end - pos), st);
        if (telemetry_ && uint64_t(samples_) % kTelemetryQuantum == 0) publishTelemetry(*p);
        pos = end;
    }
    if (total > count) {
        // Overflow: latest value per parameter, applied at the end of the call, never blocking.
        std::array<bool, std::size_t(ParamTarget::Count)> seen{};
        for (uint32_t k = total; k-- > count;) {
            const auto t = std::size_t(b.events[k].target);
            if (t >= seen.size() || seen[t]) continue;
            seen[t] = true;
            applyEvent(b.events[k]);
        }
        bump(eventsCoalesced_);
        st |= status::kEventsCoalesced;
    }
    r.frames = frames;
    r.status = finishStatus(*p, st);
    captureBoundary();
    return r;
}

ProcessResult Engine::process(const ProcessContext& ctx, const AudioBuses& buses,
                              std::span<const state::TransactionEvent> events) noexcept {
    ProcessResult r;
    if (!validateCall(ctx, buses, r)) return r;
    PreparedEngine* p = active_.get();
    uint32_t st = 0;
    graphBoundary(*p, st);
    callBoundary(*p, ctx, buses, st);
    const uint32_t frames = ctx.frames;
    const std::size_t count = events.size();
    std::size_t ev = 0;
    uint32_t pos = 0;
    for (;;) {
        while (ev < count && std::min(events[ev].sampleOffset, frames) <= pos) applyTransaction(*p, events[ev++]);
        if (pos >= frames) break;
        uint32_t end = frames;
        if (ev < count) end = std::min(end, std::min(events[ev].sampleOffset, frames));
        end = sliceLimit(pos, std::min(end, pos + uint32_t(p->maxBlock_)));
        runSlice(*p, buses, int(pos), int(end - pos), st);
        if (telemetry_ && uint64_t(samples_) % kTelemetryQuantum == 0) publishTelemetry(*p);
        pos = end;
    }
    for (; ev < count; ++ev) applyTransaction(*p, events[ev]);
    r.frames = frames;
    r.status = finishStatus(*p, st);
    captureBoundary();
    return r;
}

ProcessResult Engine::processQueued(const ProcessContext& ctx, const AudioBuses& buses, state::TransactionQueue& q) noexcept {
    return process(ctx, buses, q.drainSlice(ctx.frames));
}

void Engine::reset() noexcept {
    if (!active_) return;
    PreparedEngine& p = *active_;
    if (fading_) {
        fading_ = false; oldGraph_ = nullptr; fadePos_ = 0;
        if (graphs_ && curGraph_) graphs_->releasedBelow_.store(curGraph_->generation(), std::memory_order_release);
    }
    p.circuit_.reset();
    if (curGraph_) curGraph_->circuit().reset();
    p.output_.reset();
    MacroRuntime& m = p.macro_;
    m.smoother.prepare(p.fs_, m.targets, 4, 0.020);
    blueprint::TimelineSnapshot ts = m.timeline.snapshot();
    ts.smoothed = ts.target; ts.rampLeft = 0; ts.rampStep = 0;
    m.timeline.restore(ts);
    for (auto& z : m.z) z[0] = z[1] = 0.0;
    // Overlays: pristine prepared state (zero stochastic history, detectors idle, tempo unlocked,
    // fault channels nominal), then the current parameter targets and applied fault overlays.
    const OverlayParams prm = p.overlay_.prm;
    p.overlay_ = p.overlayInit_;
    p.overlay_.prm = prm;
    configureSidechain(p.overlay_, p.fs_);
    configureTempo(p.overlay_);
    for (int k = 0; k < kMaxFaultSlots; ++k) {
        if (!p.faultAvail_[k]) continue;
        blueprint::FaultOverlay f = prm.faults[k];
        f.revision = std::max(f.revision, p.overlay_.faults[k].overlay().revision);
        p.overlay_.faults[k].apply(f);
    }
    for (auto& d : p.pitch_) d.reset();
    for (auto& d : p.driftDry_) d.reset();
    const double v[5] = {m.targets[0], m.targets[1], m.targets[2], m.targets[3], m.timeline.smoothedU()};
    evaluateWithOverlays(p, v);
}

// ---------------------------------------------------------------------------------------------
// Graphs

GraphTarget Engine::graphTarget() const noexcept {
    GraphTarget t;
    if (!active_) return t;
    const PreparedEngine& p = *active_;
    t.sampleRate = p.fs_; t.maxBlock = p.maxBlock_; t.channels = p.channels_;
    t.os = p.os_; t.maxNewton = p.quality_.maxNewton; t.padToFamilyMax = p.pad_;
    t.circuitLatency = p.quality_.circuitLatency;
    return t;
}

bool Engine::attachGraphInstaller(GraphInstaller* g) noexcept {
    if (!g) { dropGraphs(); graphs_ = nullptr; return true; }
    if (!active_) return false;
    const GraphTarget t = graphTarget(), gt = g->target();
    if (t.sampleRate != gt.sampleRate || t.channels != gt.channels || t.maxBlock != gt.maxBlock || t.os.factor != gt.os.factor ||
        t.os.family != gt.os.family || t.os.passFraction != gt.os.passFraction || t.os.stopbandDb != gt.os.stopbandDb ||
        t.maxNewton != gt.maxNewton || t.padToFamilyMax != gt.padToFamilyMax || t.circuitLatency != gt.circuitLatency)
        return false;
    if (active_->quality_.circuitLatency > active_->fadeLen_) return false;   // dry hand-over needs latency <= fade
    dropGraphs();
    graphs_ = g;
    return true;
}

void Engine::dropGraphs() noexcept {
    fading_ = false; fadePos_ = 0;
    oldGraph_ = nullptr;
    curGraph_ = nullptr;
    if (graphs_) {
        const uint64_t inst = graphs_->pool_.installedGenerationAudio();
        const uint64_t prev = graphs_->releasedBelow_.load(std::memory_order_relaxed);
        graphs_->releasedBelow_.store(std::max(prev, inst + 1), std::memory_order_release);
    }
}

// ---------------------------------------------------------------------------------------------
// Snapshots

void Engine::writeState(StateWriter& w) const noexcept {
    const PreparedEngine& p = *active_;
    StateHeader h;
    h.samples = samples_; h.epoch = epoch_; h.haveEpoch = haveEpoch_ ? 1 : 0;
    h.channels = p.channels_; h.maxBlock = p.maxBlock_;
    w.pod(h);
    w.pod(p.macro_);
    w.pod(p.overlay_);
    currentCircuit().saveState(w);
    p.output_.saveState(w);
    for (int c = 0; c < p.channels_; ++c) p.driftDry_[c].save(w);
    // v3: pitch-drift delay ring/slewed delay per channel (length-prefixed; scratch is preallocated).
    if (p.driftActive_) {
        for (int c = 0; c < p.channels_; ++c) {
            const std::uint64_t n = p.pitch_[c].stateSize();
            w.pod(n);
            if (n <= p.pitchState_.size()) {
                p.pitch_[c].getState(p.pitchState_.data());
                w.doubles(p.pitchState_.data(), std::size_t(n));
            }
        }
    }
}

bool Engine::readState(StateReader& r, bool apply) noexcept {
    PreparedEngine& p = *active_;
    StateHeader h;
    if (!r.pod(h) || h.magic != kStateMagic || h.channels != p.channels_ || h.maxBlock != p.maxBlock_ || h.samples < 0) return false;
    MacroRuntime m;
    if (!r.pod(m)) return false;
    if (!(m.drive > 0 && std::isfinite(m.drive)) || m.smoother.size() != 4) return false;
    OverlayRuntime o;
    if (!r.pod(o)) return false;
    const double fin[] = {o.b, o.P, o.iExt, o.iLoad, o.tempoDrift, o.modQ, o.evalQ, o.prm.transientAmount, o.prm.sidechainDepth,
                          o.prm.sidechainHpfHz, o.prm.sidechainBudgetAmps, o.prm.tempoDepth, o.tempo.ppq(), o.tempo.bpm(), o.motion.q()};
    for (double x : fin) if (!std::isfinite(x)) return false;
    if (o.prm.sidechainSource > 2 || o.prm.tempoDivision > 5 || o.prm.tempoModifier > 2 || o.prm.tempoEnabled > 1 || o.hookActive < 0 ||
        o.hookActive > 1)
        return false;
    for (int k = 0; k < kMaxFaultSlots; ++k)
        if (p.faultAvail_[k] && !blueprint::validOverlay(o.prm.faults[k])) return false;
    if (!currentCircuit(p).loadState(r, apply)) return false;
    if (!p.output_.loadState(r, apply)) return false;
    for (int c = 0; c < p.channels_; ++c) if (!p.driftDry_[c].load(r, apply)) return false;
    if (p.driftActive_) {
        for (int c = 0; c < p.channels_; ++c) {
            std::uint64_t n = 0;
            if (!r.pod(n) || n != p.pitch_[c].stateSize() || n > p.pitchState_.size()) return false;
            if (!r.doubles(p.pitchState_.data(), std::size_t(n))) return false;
            if (!p.pitch_[c].validState(p.pitchState_.data())) return false;
            if (apply && !p.pitch_[c].setState(p.pitchState_.data())) return false;
        }
    }
    if (apply) {
        samples_ = h.samples; epoch_ = h.epoch; haveEpoch_ = h.haveEpoch != 0;
        p.macro_ = m;
        p.overlay_ = o;
    }
    return r.ok();
}

std::size_t Engine::snapshotBytes() const noexcept {
    if (!active_) return 0;
    StateWriter w(nullptr, 0);
    writeState(w);
    return w.size();
}

state::SnapshotIdentity Engine::snapshotIdentity() const noexcept {
    state::SnapshotIdentity id;
    if (!active_) return id;
    const PreparedEngine& p = *active_;
    Fnv64 h;
    h.pod(p.configHash_);
    h.pod(currentCircuit().modelHash());
    id.layout.layoutId = kSnapshotLayoutId;
    id.layout.version = kSnapshotLayoutVersion;
    id.layout.byteSize = uint32_t(snapshotBytes());
    id.sampleRate = p.fs_;
    id.oversampling = uint32_t(p.quality_.factor);
    id.modelId = h.h;
    return id;
}

bool Engine::attachSnapshotExchange(state::SnapshotExchange* x) noexcept {
    if (!x) { snap_ = nullptr; return true; }
    if (!active_ || !(x->identity() == snapshotIdentity())) return false;
    snap_ = x;
    return true;
}

std::size_t Engine::captureState(std::span<std::byte> out) const noexcept {
    if (!active_ || fading_) return 0;
    const std::size_t n = snapshotBytes();
    if (out.size() != n) return 0;
    StateWriter w(out.data(), out.size());
    writeState(w);
    return w.ok() && w.size() == n ? n : 0;
}

bool Engine::loadState(std::span<const std::byte> in) noexcept {
    if (!active_ || in.size() != snapshotBytes()) return false;
    StateReader r(in.data(), in.size());
    if (!readState(r, false) || !r.atEnd()) return false;
    if (fading_) {
        fading_ = false; oldGraph_ = nullptr; fadePos_ = 0;
        if (graphs_ && curGraph_) graphs_->releasedBelow_.store(curGraph_->generation(), std::memory_order_release);
    }
    r.rewind();
    return readState(r, true);
}

state::RestoreDecision Engine::restoreSnapshot(const state::SavedSnapshot& saved, std::string& reason) {
    reason.clear();
    if (!saved.valid) { reason = "no checkpoint"; return state::RestoreDecision::NoCheckpoint; }
    if (!active_) { reason = "engine not prepared"; return state::RestoreDecision::ResetWarm; }
    const state::RestoreDecision d = state::decideSnapshotRestore(saved, snapshotIdentity(), reason);
    if (d == state::RestoreDecision::NoCheckpoint) return d;
    if (d == state::RestoreDecision::Restore) {
        if (loadState(std::span<const std::byte>(saved.payload.data(), saved.payload.size()))) return d;
        reason = "snapshot payload failed provider-layout validation";
    }
    reset();
    return state::RestoreDecision::ResetWarm;
}

bool Engine::copyStateFrom(const Engine& src, std::span<std::byte> scratch) noexcept {
    if (!active_ || !src.active_ || src.fading_) return false;
    const std::size_t n = snapshotBytes();
    if (scratch.size() < n || !(src.snapshotIdentity() == snapshotIdentity())) return false;
    if (src.captureState(scratch.first(n)) != n) return false;
    return loadState(std::span<const std::byte>(scratch.data(), n));  // v3 payload carries the pitch delay
}

// ---------------------------------------------------------------------------------------------

EngineDiagnostics Engine::diagnostics() const noexcept {
    EngineDiagnostics d;
    d.samples = samples_; d.epoch = epoch_; d.epochChanges = epochChanges_;
    d.inputNonFinite = inputNonFinite_; d.inputClamped = inputClamped_; d.eventsCoalesced = eventsCoalesced_;
    d.staleInstalls = staleInstalls_;
    d.paramEvents = paramEvents_; d.unsupportedEvents = unsupported_; d.unknownEvents = unknown_;
    d.otherSlotEvents = otherSlot_; d.configEvents = configEvents_; d.noteEvents = noteEvents_;
    d.graphGeneration = curGraph_ ? curGraph_->generation() : 0;
    d.graphInstalls = graphInstalls_; d.graphRejected = graphRejected_;
    d.graphFading = fading_; d.graphFadePos = fadePos_;
    d.snapshotsPublished = snapPublished_; d.snapshotsDeferred = snapDeferred_; d.snapshotsRefused = snapRefused_;
    if (active_) {
        const PreparedEngine& p = *active_;
        d.generation = p.generation_;
        d.graphFadeLen = p.fadeLen_;
        d.circuit = currentCircuit().diagnostics();
        d.output = p.output_.diagnostics();
        const MacroRuntime& m = p.macro_;
        d.macros = {m.smoother.value(0), m.smoother.value(1), m.smoother.value(2), m.smoother.value(3)};
        d.effective = m.eff;
        d.base = p.base_;
        d.driveGain = m.drive;
        d.lifecycle = m.timeline.smoothedU();
        const OverlayRuntime& o = p.overlay_;
        OverlayDiagnostics& x = d.overlay;
        x.transientEvidence = o.b; x.transientStrength = o.P;
        x.sidechainCurrentA = o.iExt; x.loadCurrentA = o.iLoad;
        x.sidechainSource = int(o.prm.sidechainSource);
        x.sidechainConnected = scConnected_;
        x.sidechainDisconnected = !scConnected_ && o.prm.sidechainSource != std::uint32_t(control::SidechainSource::Main);
        x.sidechainSanitized = o.sidechain.status().sanitized;
        x.transientSanitized = o.transient.sawNonFinite();
        x.tempoEnabled = o.prm.tempoEnabled != 0;
        x.bpm = o.tempo.bpm(); x.ppq = o.tempo.ppq(); x.tempoPhase = o.tempo.phase();
        x.recoveryCoefficient = o.tempo.recoveryCoefficient(); x.tempoDrift = o.tempoDrift;
        x.tempoStatus = o.tempo.status();
        x.tempoResets = o.tempoResets;
        x.driftDelayActive = p.driftActive_;
        x.driftLatency = p.quality_.driftLatency;
        x.motionQ = o.motion.q(); x.modulationQ = o.modQ;
        x.driftDelaySamples = p.driftActive_ ? p.pitch_[0].currentDelaySamples() : 0.0;
        for (int k = 0; k < kMaxFaultSlots; ++k) {
            auto& f = x.faults[k];
            f.used = p.faultUsed_[k];
            f.available = p.faultAvail_[k];
            f.reason = p.faultReason_[k] ? p.faultReason_[k] : "";
            f.cleared = !p.faultAvail_[k] || o.faults[k].cleared();
            f.overlay = o.prm.faults[k];
            f.out = o.faultOut[k];
            f.transitions = p.faultAvail_[k] ? o.faults[k].contactTransitions() : 0;
            f.partial = p.faultPartial_[k] ? p.faultPartial_[k] : "";
            f.element = p.faultElement_[k];
            f.parameter = p.faultParam_[k];
            f.nominal = p.faultNominal_[k];
            f.applied = p.faultParam_[k] >= 0 ? p.circuit_.parameter(0, p.faultElement_[k], p.faultParam_[k]) : 0.0;
        }
        x.faultParamUpdates = p.circuit_.parameterUpdates();
        x.faultParamRefused = p.circuit_.parameterRefusals();
        if (curGraph_ && p.trackCount_ > 0)
            x.faultPathReason = "graph path active: graph solvers map elements differently, faults apply to the built-in circuit only";
        x.faultActive = o.hookActive != 0;
        x.faultHook = o.hook;
        x.faultApplied = o.faultApplied; x.faultRejected = o.faultRejected; x.faultResets = o.faultResets;
    }
    d.overlay.telemetryAttached = telemetry_ != nullptr;
    d.overlay.telemetryFrames = telemetryFrames_;
    if (telemetry_) {
        d.overlay.telemetryPublished = telemetry_->published();
        d.overlay.telemetryDropped = telemetry_->droppedFull();
        d.overlay.telemetryDiscarded = telemetry_->discardedNoReader();
    }
    return d;
}

}  // namespace cd::runtime
