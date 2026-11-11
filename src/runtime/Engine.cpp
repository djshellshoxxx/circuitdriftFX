// CircuitDrift runtime: integrated engine implementation (see Engine.h). Owner: cd_runtime.
#include "runtime/Engine.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
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
                   kHpfEnabled = 1010, kHpfFrequency = 1011, kQualityMode = 1012, kQualityZeroLatency = 1015,
                   kPitchDrift = 1016, kSlotALifecycle = 1018, kSlotAScenario = 1019, kSlotBLifecycle = 1020,
                   kSlotBScenario = 1021, kScSource = 1022, kScDepth = 1023, kScHpf = 1024, kTransientPreserve = 1025;

constexpr uint64_t kStateMagic = 0x3254535f454e4443ull;   // "CDNE_ST2"

struct StateHeader {
    uint64_t magic = kStateMagic;
    int64_t samples = 0;
    uint64_t epoch = 0;
    int64_t haveEpoch = 0, channels = 0, maxBlock = 0;
};

static_assert(std::is_trivially_copyable_v<MacroRuntime>, "MacroRuntime is part of the snapshot layout");

double clamp01(double x) noexcept { return std::isfinite(x) ? std::clamp(x, 0.0, 1.0) : 0.0; }

// RBJ cookbook low-pass (Q = 1/sqrt2: 2nd-order Butterworth), normalized a0 = 1, TDF-II.
control::BiquadCoeffs designLowpass(double fs, double fc) noexcept {
    control::BiquadCoeffs c;
    if (!(fs > 0) || !std::isfinite(fc)) return c;
    fc = std::clamp(fc, 20.0, 0.45 * fs);
    const double w0 = 2.0 * M_PI * fc / fs, cs = std::cos(w0), al = std::sin(w0) / (2.0 * M_SQRT1_2);
    const double a0 = 1.0 + al;
    c.b0 = (1.0 - cs) * 0.5 / a0;
    c.b1 = (1.0 - cs) / a0;
    c.b2 = c.b0;
    c.a1 = -2.0 * cs / a0;
    c.a2 = (1.0 - al) / a0;
    return c;
}

// Re-evaluates effective macro values from smoothed values v = {a,s,d,w,u,q}.
void evaluateMacros(MacroRuntime& m, const control::ManifestBindings& mbIn, const control::BaseValues& base, double fs,
                    const double* v) noexcept {
    control::ManifestBindings mb = mbIn;
    mb.analogNoise = m.analogNoise != 0;   // runtime flag (analogNoise.enabled) is the source of truth
    const blueprint::Condition cond = m.timeline.condition();
    control::TimelineOffsets tl;
    tl.serviceAgeYears = cond.values.ageYears;
    tl.tolSigma = cond.values.toleranceSigma;
    tl.leakFrac = cond.values.leakageFraction;
    tl.driveDb = 0.0;
    tl.biasOffset = cond.values.biasOffsetSpan * mb.biasSpan;
    control::ModulationInput mod;
    mod.q = v[5];
    control::Hooks hooks;
    hooks.timeline = &tl;
    hooks.mod = &mod;
    m.eff = control::evaluate(base, control::MacroValues{v[0], v[1], v[2], v[3]}, mb, hooks);
    m.drive = m.eff.supportsWarmth ? std::pow(10.0, m.eff.driveDb / 20.0) : 1.0;
    if (!(m.drive > 0 && std::isfinite(m.drive))) m.drive = 1.0;
    m.shelf = m.eff.supportsAge ? control::designHighShelf(fs, m.eff.shelfHz, m.eff.shelfGainDb, m.eff.shelfQ) : control::BiquadCoeffs{};
    m.noiseGain = m.eff.noiseEnabled ? std::pow(10.0, m.eff.noiseDbfs / 20.0) : 0.0;
    if (!std::isfinite(m.noiseGain)) m.noiseGain = 0.0;
    for (int k = 0; k < 6; ++k) m.last[k] = v[k];
}

control::SidechainLoadDetector::Config sidechainConfig(double hpfHz) noexcept {
    // Configured External with unit depth/budget: process() returns clamp(E/E_ref,0,4); the engine
    // applies the smoothed depth, the manifest I_budget and the selected source itself.
    control::SidechainLoadDetector::Config c;
    c.source = control::SidechainLoadDetector::Source::External;
    c.hpfHz = hpfHz; c.depth = 1.0; c.iBudgetAmps = 1.0;
    return c;
}

void seedCreative(MacroRuntime& m, double fs, std::uint64_t cut, std::uint64_t del, std::uint64_t noise) noexcept {
    m.cutMotion.prepare(fs, cut);
    m.delayMotion.prepare(fs, del);
    m.noiseRng.seed(noise);
}

void initMacros(MacroRuntime& m, const EngineConfig& cfg, const control::ManifestBindings& mb, double fs) noexcept {
    const double init[4] = {clamp01(cfg.initialMacros.age), clamp01(cfg.initialMacros.sag), clamp01(cfg.initialMacros.drift),
                            clamp01(cfg.initialMacros.warmth)};
    m.smoother.prepare(fs, init, 4, 0.020);
    for (int k = 0; k < 4; ++k) m.targets[k] = init[k];
    m.dynTargets[0] = clamp01(cfg.dynamics.transientPreserve);
    m.dynTargets[1] = std::isfinite(cfg.dynamics.sidechainDepth) ? std::clamp(cfg.dynamics.sidechainDepth, 0.0, 2.0) : 0.0;
    m.dyn.prepare(fs, m.dynTargets, 2, 0.020);
    m.timeline.prepare(fs);
    m.timeline.setScenario(cfg.initialScenario);
    blueprint::TimelineSnapshot ts = m.timeline.snapshot();
    ts.target = ts.smoothed = clamp01(cfg.initialLifecycle);
    ts.rampLeft = 0; ts.rampStep = 0;
    m.timeline.restore(ts);
    m.analogNoise = cfg.analogNoise ? 1 : 0;
    m.transient.prepare(fs);
    m.scHpfHz = std::isfinite(cfg.dynamics.sidechainHpfHz) ? std::clamp(cfg.dynamics.sidechainHpfHz, 20.0, 300.0) : 40.0;
    m.sidechain.prepare(fs, sidechainConfig(m.scHpfHz));
    m.scSource = std::int64_t(cfg.dynamics.sidechainSource);
    m.cutOct = m.lpfOct = 0.0;
    m.lpf = designLowpass(fs, cfg.creative.cutoffHz);
    for (auto& z : m.lz) z[0] = z[1] = 0.0;
    const double v[6] = {init[0], init[1], init[2], init[3], m.timeline.smoothedU(), 0.0};
    evaluateMacros(m, mb, cfg.base, fs, v);
    for (auto& z : m.z) z[0] = z[1] = 0.0;
}

void hashBindings(Fnv64& h, const control::ManifestBindings& b) noexcept {
    const bool flags[5] = {b.hasAge, b.hasSag, b.hasWarmth, b.hasDrift, b.analogNoise};
    h.bytes(flags, sizeof flags);
    const double d[7] = {b.cBase, b.cMin, b.biasSpan, b.leakRange, b.maxTolSigma, b.fluxMin, b.fluxMax};
    h.bytes(d, sizeof d);
}

}  // namespace

control::ManifestBindings syntheticMacroBindings() noexcept {
    control::ManifestBindings b;
    b.hasAge = true;      // creative HF shelf (not an ageing equation)
    b.hasWarmth = true;   // pre-stage drive dB only (no bias span / flux domain in the fixtures)
    return b;
}

std::string qualityLabel(const QualityInfo& q) {
    char buf[96];
    const char* fam = q.family == dsp::OsFamily::Iir ? "IIR" : "FIR";
    if (q.zeroLatency) {
        std::snprintf(buf, sizeof buf, "Zero Latency %dx (Draft)", q.factor);
        return buf;
    }
    const char* mode = q.mode == QualityMode::Eco ? "Eco" : q.mode == QualityMode::Studio ? "Studio"
                     : q.mode == QualityMode::Render ? "Render" : "Manual";
    std::snprintf(buf, sizeof buf, "%s %dx %s%s%s", mode, q.factor, fam, q.draft ? " (Draft)" : "",
                  q.familyException ? " (low-latency exception)" : "");
    return buf;
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
    i.mode = q.mode;
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
    // Creative stages (labelled creative): Drift cutoff motion filter and optional pitch-drift delay.
    if (cfg.creative.cutoffMotion && !(std::isfinite(cfg.creative.cutoffHz) && cfg.creative.cutoffHz >= 20.0 &&
                                       cfg.creative.cutoffHz <= 0.45 * cfg.sampleRate)) {
        err = "creative cutoff outside 20 Hz .. 0.45 Fs";
        return nullptr;
    }
    p->cutoffOn_ = cfg.creative.cutoffMotion;
    p->cutoffHz_ = cfg.creative.cutoffHz;
    p->cutSlewA_ = std::exp(-1.0 / (kCutoffSlewSeconds * cfg.sampleRate));
    p->pitchOn_ = cfg.creative.pitchDelay && !cfg.quality.zeroLatency;
    p->quality_.pitchDelay = p->pitchOn_;
    p->quality_.pitchDelayUnavailable = cfg.creative.pitchDelay && cfg.quality.zeroLatency;
    int pitchLatency = 0;
    for (int c = 0; c < cfg.channels; ++c) {
        if (p->pitchOn_) {
            if (!p->pitch_[c].prepare(cfg.sampleRate)) { err = "pitch-drift delay rejected sample rate"; return nullptr; }
            p->pitch_[c].setEnabled(true);
            const double exact = control::PitchDriftDelay::kBaseMs * 1e-3 * cfg.sampleRate;
            pitchLatency = int(std::lround(exact));
            p->quality_.pitchResidual = double(pitchLatency) - exact;
        }
        p->pitchDry_[c].prepare(pitchLatency);
    }
    p->quality_.pitchLatency = pitchLatency;
    OutputConfig oc;
    oc.sampleRate = cfg.sampleRate; oc.maxBlock = cfg.maxBlock; oc.channels = cfg.channels;
    oc.clipOs = spec; oc.padToFamilyMax = !cfg.quality.zeroLatency;
    oc.upstreamLatency = p->circuit_.latencySamples() + pitchLatency; oc.initial = cfg.initial;
    if (!p->output_.prepare(oc, err)) return nullptr;
    p->quality_.circuitLatency = p->circuit_.latencySamples();
    p->quality_.clipLatency = p->output_.latencySamples();
    p->quality_.latencySamples = p->output_.totalLatencySamples();
    p->os_ = spec; p->pad_ = !cfg.quality.zeroLatency;
    p->fs_ = cfg.sampleRate; p->channels_ = cfg.channels; p->maxBlock_ = cfg.maxBlock;
    const std::size_t sz = std::size_t(cfg.channels) * std::size_t(cfg.maxBlock), M = std::size_t(cfg.maxBlock);
    p->raw_.assign(sz, 0.0); p->gained_.assign(sz, 0.0); p->wet_.assign(sz, 0.0); p->dry_.assign(sz, 0.0);
    p->wet2_.assign(sz, 0.0); p->dry2_.assign(sz, 0.0); p->xout_.assign(sz, 0.0);
    p->drive_.assign(M, 1.0);
    p->shelf_.assign(M * 5, 0.0);
    p->lpfc_.assign(M * 5, 0.0);
    for (auto* v : {&p->cEff_, &p->sag_, &p->pres_, &p->iext_, &p->sens_, &p->dsm_, &p->qd_, &p->ng_}) v->assign(M, 0.0);
    // Bindings: Sag from the supply manifest, Drift from tempco feedback / creative stages.
    p->bindings_ = cfg.macroBindings;
    const SyntheticSupply& sup = p->circuit_.supply();
    p->supplyOn_ = sup.enabled;
    p->iBudget_ = sup.enabled ? sup.iBudgetA : 0.0;
    p->bindings_.hasSag = sup.enabled;
    p->bindings_.cBase = sup.enabled ? sup.cBaseF : 0.0;
    p->bindings_.cMin = sup.enabled ? sup.cMinF : 0.0;
    p->bindings_.hasDrift = p->circuit_.hasTemperatureFeedback() || p->cutoffOn_ || p->pitchOn_;
    p->bindings_.analogNoise = cfg.analogNoise;
    p->base_ = cfg.base;
    const uint32_t slot = cfg.slotRole == core::ParamScope::SlotB ? 1u : 0u;
    p->cutSeed_ = control::deriveSeed(cfg.seed, slot, "creative.cutoff", control::StreamType::CutoffMotion);
    p->delaySeed_ = control::deriveSeed(cfg.seed, slot, "creative.pitchDelay", control::StreamType::DelayMotion);
    p->noiseSeed_ = control::deriveSeed(cfg.seed, slot, "analog.noise", control::StreamType::Other);
    seedCreative(p->macro_, cfg.sampleRate, p->cutSeed_, p->delaySeed_, p->noiseSeed_);
    initMacros(p->macro_, cfg, p->bindings_, cfg.sampleRate);
    p->fadeLen_ = int(std::max(1.0, std::round(kGraphFadeSeconds * cfg.sampleRate)));
    Fnv64 h;
    h.str("cd.runtime.Engine/2");
    h.bytes(state::kEngineModelVersion.data(), state::kEngineModelVersion.size());
    h.pod(cfg.channels); h.pod(p->quality_.factor); h.pod(p->quality_.family); h.pod(p->quality_.maxNewton);
    h.pod(p->quality_.zeroLatency); h.pod(p->quality_.latencySamples);
    hashBindings(h, p->bindings_);
    h.pod(p->cutoffOn_); h.pod(p->cutoffHz_); h.pod(p->pitchOn_);
    p->configHash_ = h.h;
    p->generation_ = ++nextGeneration_;
    err.clear();
    return p;
}

bool Engine::tryInstallPrepared(std::unique_ptr<PreparedEngine>& h) noexcept {
    if (!h) return false;
    if (active_ && h->generation_ <= active_->generation_) { bump(staleInstalls_); return false; }
    if (incoming_ || retired_) return false;   // a quality transition owns the paths; finish/reclaim first
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
    incoming_.reset(); retired_.reset(); pending_.reset();   // non-audio: abandon any transition
    if (!tryInstallPrepared(p)) { err = "stale generation"; return false; }
    p.reset();  // reclaim previous engine here (non-audio caller)
    role_ = cfg.slotRole;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Events

void Engine::applyEvent(PreparedEngine& p, const ParamEvent& e) noexcept {
    OutputStage& o = p.output_;
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
        default: break;
    }
}

void Engine::applyHostParam(PreparedEngine& p, uint32_t hostId, double normalized, bool count) noexcept {
    auto cnt = [count](std::int64_t& c) noexcept { if (count) bump(c); };
    const core::ParamSpec* spec = core::findByHostId(hostId);
    if (!spec) { cnt(unknown_); return; }
    if (spec->scope != core::ParamScope::Global && spec->scope != role_) { cnt(otherSlot_); return; }
    cnt(paramEvents_);
    const double v = core::toPlain(*spec, normalized);
    const bool on = v >= 0.5;
    OutputStage& o = p.output_;
    MacroRuntime& m = p.macro_;
    switch (hostId) {
        case kMacroAge: case kMacroSag: case kMacroDrift: case kMacroWarmth:
            m.targets[hostId - kMacroAge] = clamp01(v);
            m.smoother.setTargets(m.targets, 4);
            if ((hostId == kMacroSag && !p.bindings_.hasSag) || (hostId == kMacroDrift && !p.bindings_.hasDrift))
                cnt(unsupported_);   // stored, neutral: no binding in this circuit
            break;
        case kAnalogNoise:
            m.analogNoise = on ? 1 : 0;
            m.last[0] = std::numeric_limits<double>::quiet_NaN();   // force re-evaluation next sample
            if (!p.bindings_.hasAge) cnt(unsupported_);
            break;
        case kInputGain: o.setInputGainDb(v); break;
        case kOutputGain: o.setOutputTrimDb(v); break;
        case kMix: o.setMix(v); break;
        case kGainMatch: o.setAutoGain(on); break;
        case kSoftClip: o.setSoftClipEnabled(on); break;
        case kHpfEnabled: o.setHpfEnabled(on); break;
        case kHpfFrequency: o.setHpfCutoffHz(v); break;
        case kSlotALifecycle: case kSlotBLifecycle: m.timeline.setTarget(v); break;
        case kSlotAScenario: case kSlotBScenario: m.timeline.setScenario(blueprint::Scenario(int(v))); break;
        case kScSource:
            m.scSource = std::clamp<std::int64_t>(std::int64_t(std::lround(v)), 0, 2);
            if (!p.supplyOn_) cnt(unsupported_);
            break;
        case kScDepth:
            m.dynTargets[1] = std::isfinite(v) ? std::clamp(v, 0.0, 2.0) : m.dynTargets[1];
            m.dyn.setTargets(m.dynTargets, 2);
            if (!p.supplyOn_) cnt(unsupported_);
            break;
        case kScHpf:
            if (std::isfinite(v)) {
                m.scHpfHz = std::clamp(v, 20.0, 300.0);
                m.sidechain.prepare(p.fs_, sidechainConfig(m.scHpfHz));   // noexcept; envelope restarts
            }
            if (!p.supplyOn_) cnt(unsupported_);
            break;
        case kTransientPreserve:
            m.dynTargets[0] = clamp01(v);
            m.dyn.setTargets(m.dynTargets, 2);
            if (!p.supplyOn_) cnt(unsupported_);
            break;
        default:
            if (hostId >= kQualityMode && hostId <= kQualityZeroLatency) { cnt(qRequests_); break; }   // adapter builds off-thread
            if (hostId == kPitchDrift) { cnt(pitchRequests_); break; }   // prepare-time + latency handshake
            cnt(unsupported_);
            break;
    }
}

void Engine::applyTransaction(PreparedEngine& p, const state::TransactionEvent& e, bool count) noexcept {
    switch (e.kind) {
        case state::EventKind::Param: applyHostParam(p, e.hostId, e.normalized, count); break;
        case state::EventKind::ConfigPublish: if (count) bump(configEvents_); break;
        case state::EventKind::NoteOff:
        case state::EventKind::AllNotesOff: if (count) bump(noteEvents_); break;
        default: if (count) bump(unknown_); break;
    }
}

void Engine::applyEverywhere(const state::TransactionEvent& e) noexcept {
    if (incoming_) applyTransaction(*incoming_, e, false);
    applyTransaction(*active_, e, true);
}

void Engine::applyEverywhere(const ParamEvent& e) noexcept {
    if (incoming_) applyEvent(*incoming_, e);
    applyEvent(*active_, e);
}

// ---------------------------------------------------------------------------------------------
// Processing

CircuitRuntime& Engine::currentCircuit(PreparedEngine& p) noexcept { return curGraph_ ? curGraph_->circuit() : p.circuit_; }
const CircuitRuntime& Engine::currentCircuit() const noexcept { return curGraph_ ? curGraph_->circuit() : active_->circuit_; }

void Engine::macroSlice(PreparedEngine& p, const AudioBuses& buses, int off, int n, uint32_t& st) noexcept {
    MacroRuntime& m = p.macro_;
    const std::size_t M = std::size_t(p.maxBlock_);
    double* b0 = p.shelf_.data(); double* b1 = b0 + M; double* b2 = b1 + M; double* a1 = b2 + M; double* a2 = a1 + M;
    double* l0 = p.lpfc_.data(); double* l1 = l0 + M; double* l2 = l1 + M; double* la1 = l2 + M; double* la2 = la1 + M;
    const double* g0 = p.gained_.data();
    const double* g1 = p.channels_ > 1 ? g0 + M : g0;
    const bool sideOk = buses.side && buses.sideChannels >= 1 && buses.sideChannels <= 2 && buses.side[0] &&
                        (buses.sideChannels == 1 || buses.side[1]);
    const double* sl = sideOk ? buses.side[0] + off : nullptr;
    const double* sr = sideOk ? (buses.sideChannels > 1 ? buses.side[1] + off : sl) : nullptr;
    for (int i = 0; i < n; ++i) {
        m.smoother.step();
        m.dyn.step();
        const double u = m.timeline.tick();
        m.cutMotion.advanceSample();     // advanced every sample: block-independent, seed-reproducible
        m.delayMotion.advanceSample();
        const double q = p.cutoffOn_ ? m.cutMotion.q() : 0.0;
        const double v[6] = {m.smoother.value(0), m.smoother.value(1), m.smoother.value(2), m.smoother.value(3), u, q};
        if (v[0] != m.last[0] || v[1] != m.last[1] || v[2] != m.last[2] || v[3] != m.last[3] || v[4] != m.last[4] ||
            v[5] != m.last[5])
            evaluateMacros(m, p.bindings_, p.base_, p.fs_, v);
        const std::size_t k = std::size_t(i);
        p.drive_[k] = m.drive;
        b0[i] = m.shelf.b0; b1[i] = m.shelf.b1; b2[i] = m.shelf.b2; a1[i] = m.shelf.a1; a2[i] = m.shelf.a2;
        // Supply / temperature-law controls.
        p.cEff_[k] = m.eff.supportsSag ? m.eff.reservoirC : p.bindings_.cBase;
        p.sag_[k] = m.eff.supportsSag ? v[1] : 0.0;
        p.sens_[k] = m.eff.driftSensitivity;
        p.dsm_[k] = m.eff.supportsDrift ? v[2] : 0.0;
        p.qd_[k] = m.delayMotion.q();
        p.ng_[k] = m.noiseGain;
        // Creative cutoff motion: slew the held q contribution, redesign only on change.
        if (p.cutoffOn_) {
            const double tgt = m.eff.cutoffMotionOct;
            m.cutOct = tgt + p.cutSlewA_ * (m.cutOct - tgt);
            if (std::fabs(m.cutOct - tgt) <= 1e-12) m.cutOct = tgt;
            if (m.cutOct != m.lpfOct) {
                m.lpf = designLowpass(p.fs_, p.cutoffHz_ * std::exp2(m.cutOct));
                m.lpfOct = m.cutOct;
            }
            l0[i] = m.lpf.b0; l1[i] = m.lpf.b1; l2[i] = m.lpf.b2; la1[i] = m.lpf.a1; la2[i] = m.lpf.a2;
        }
        // Detectors at host rate (linked), after macro targets and before the electrical solve.
        if (p.supplyOn_) {
            m.lastB = m.transient.process(g0[i], g1[i]);
            m.lastP = control::TransientDetector::strength(m.dyn.value(0), m.lastB);
            p.pres_[k] = m.lastP;
            double ratio = 0.0;
            if (m.scSource != std::int64_t(LoadSource::Main)) {
                m.sidechain.setConnected(sideOk);
                ratio = m.sidechain.process(sideOk ? sl[i] : 0.0, sideOk ? sr[i] : 0.0);
                if (!sideOk) st |= status::kSidechainMissing;
            }
            m.scConnected = sideOk ? 1 : 0;
            m.lastIExt = m.dyn.value(1) * p.iBudget_ * ratio;
            p.iext_[k] = m.lastIExt;
        } else {
            p.pres_[k] = 0.0;
            p.iext_[k] = 0.0;
        }
    }
}

void Engine::wetStages(PreparedEngine& p, double* const* wet, double* const* dry, int n) noexcept {
    const int C = p.channels_;
    const std::size_t M = std::size_t(p.maxBlock_);
    MacroRuntime& m = p.macro_;
    auto biquad = [&](const double* coef, double (&z)[2], double* w) noexcept {
        const double* b0 = coef; const double* b1 = b0 + M; const double* b2 = b1 + M;
        const double* a1 = b2 + M; const double* a2 = a1 + M;
        double z0 = z[0], z1 = z[1];
        for (int i = 0; i < n; ++i) {
            const double x = w[i];
            const double y = b0[i] * x + z0;
            z0 = b1[i] * x - a1[i] * y + z1;
            z1 = b2[i] * x - a2[i] * y;
            w[i] = y;
        }
        if (!isFiniteSample(z0) || !isFiniteSample(z1)) { z0 = z1 = 0.0; }
        z[0] = z0; z[1] = z1;
    };
    // Age: creative HF shelf on the wet path (TDF-II, per-sample coefficients).
    if (p.bindings_.hasAge)
        for (int c = 0; c < C; ++c) biquad(p.shelf_.data(), m.z[c], wet[c]);
    // Drift: creative low-pass with seeded cutoff motion (+-0.15 d octaves).
    if (p.cutoffOn_)
        for (int c = 0; c < C; ++c) biquad(p.lpfc_.data(), m.lz[c], wet[c]);
    // Optional creative pitch-drift delay; dry delayed by the declared integer base latency.
    if (p.pitchOn_)
        for (int c = 0; c < C; ++c)
            for (int i = 0; i < n; ++i) {
                wet[c][i] = p.pitch_[c].process(wet[c][i], p.dsm_[std::size_t(i)], p.qd_[std::size_t(i)]);
                dry[c][i] = p.pitchDry_[c].process(dry[c][i]);
            }
    // Analog Noise (Age, gated by analogNoise.enabled): seeded Gaussian, RMS -90+24a dBFS, wet only.
    for (int i = 0; i < n; ++i) {
        const double g = p.ng_[std::size_t(i)];
        if (g > 0.0)
            for (int c = 0; c < C; ++c) wet[c][i] += g * m.noiseRng.gaussian();
    }
}

void Engine::runPath(PreparedEngine& p, const double* const* in, const AudioBuses& buses, int off, double* const* o, int n,
                     uint32_t& st, bool primary) noexcept {
    const int C = p.channels_;
    const std::size_t M = std::size_t(p.maxBlock_);
    const double* rawC[2] = {};
    double* raw[2] = {}; double* gained[2] = {}; double* wet[2] = {}; double* dry[2] = {};
    double* wet2[2] = {}; double* dry2[2] = {};
    const double *gainedC[2] = {}, *wetC[2] = {}, *dryC[2] = {};
    for (int c = 0; c < C; ++c) {
        const std::size_t base = std::size_t(c) * M;
        raw[c] = p.raw_.data() + base; gained[c] = p.gained_.data() + base;
        wet[c] = p.wet_.data() + base; dry[c] = p.dry_.data() + base;
        wet2[c] = p.wet2_.data() + base; dry2[c] = p.dry2_.data() + base;
        rawC[c] = raw[c]; gainedC[c] = gained[c]; wetC[c] = wet[c]; dryC[c] = dry[c];
        const double* x = in[c];
        for (int i = 0; i < n; ++i) {
            double v = x[i];
            if (!isFiniteSample(v)) { v = 0.0; if (primary) bump(inputNonFinite_); st |= status::kInputSanitized; }
            else if (std::fabs(v) > kEntryLimit) { v = std::copysign(kEntryLimit, v); if (primary) bump(inputClamped_); st |= status::kInputSanitized; }
            raw[c][i] = v;
        }
    }
    p.output_.applyInputGain(rawC, gained, n);
    macroSlice(p, buses, off, n, st);
    CircuitControls ctl;
    ctl.drive = p.drive_.data();
    ctl.reservoirC = p.cEff_.data(); ctl.sag = p.sag_.data(); ctl.preserve = p.pres_.data();
    ctl.extCurrent = p.iext_.data(); ctl.tempSensitivity = p.sens_.data();
    ctl.source = LoadSource(p.macro_.scSource);
    CircuitRuntime& cur = primary ? currentCircuit(p) : p.circuit_;
    if (!primary || !fading_) {
        cur.process(gainedC, wet, dry, n, ctl);
    } else {
        CircuitRuntime& old = oldGraph_ ? oldGraph_->circuit() : p.circuit_;
        old.process(gainedC, wet2, dry2, n, ctl);
        cur.process(gainedC, wet, dry, n, ctl);
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
    wetStages(p, wet, dry, n);
    p.output_.process(rawC, dryC, wetC, o, n);
    if (primary) samples_ += n;
}

void Engine::runSlice(PreparedEngine& p, const AudioBuses& buses, int off, int n, uint32_t& st) noexcept {
    const int C = p.channels_;
    const double* in[2] = {};
    double* out[2] = {};
    for (int c = 0; c < C; ++c) { in[c] = buses.in[c] + off; out[c] = buses.out[c] + off; }
    if (!incoming_) {
        runPath(p, in, buses, off, out, n, st, true);
        return;
    }
    // Live quality transition: the incoming path runs first (reads the input before an in-place
    // primary overwrites it), then the active path; pre-roll, then a 20 ms linear equal-sum fade.
    PreparedEngine& q = *incoming_;
    double* xo[2] = {};
    for (int c = 0; c < C; ++c) xo[c] = q.xout_.data() + std::size_t(c) * std::size_t(q.maxBlock_);
    runPath(q, in, buses, off, xo, n, st, false);
    runPath(p, in, buses, off, out, n, st, true);
    const double invLen = 1.0 / double(qLen_);
    for (int i = 0; i < n; ++i) {
        if (qWarm_ > 0) { --qWarm_; continue; }   // pre-roll: incoming delay lines fill, old path audible
        if (qPos_ < qLen_) {
            ++qPos_;
            const double x = qPos_ == qLen_ ? 1.0 : double(qPos_) * invLen;
            for (int c = 0; c < C; ++c) out[c][i] = (1.0 - x) * out[c][i] + x * xo[c][i];
        } else {
            for (int c = 0; c < C; ++c) out[c][i] = xo[c][i];
        }
    }
    st |= status::kQualityTransition;
    if (qWarm_ == 0 && qPos_ >= qLen_) finishQualityFade();
}

void Engine::finishQualityFade() noexcept {
    retired_ = std::move(active_);      // requestQuality guaranteed retired_ was empty
    active_ = std::move(incoming_);
    qWarm_ = 0; qPos_ = 0;
    bump(qCompleted_);
}

void Engine::graphBoundary(PreparedEngine& p, uint32_t& st) noexcept {
    if (!graphs_ || fading_ || incoming_) return;
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
    if (fading_ || incoming_) { bump(snapDeferred_); return; }
    if (active_->pitchOn_) { bump(snapRefused_); return; }
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
    if (haveEpoch_ && ctx.epoch != epoch_) bump(epochChanges_);
    epoch_ = ctx.epoch; haveEpoch_ = true;
    return true;
}

uint32_t Engine::finishStatus(PreparedEngine& p, uint32_t st) noexcept {
    const auto cd = currentCircuit(p).diagnostics();
    const auto od = p.output_.diagnostics();
    if (cd.fallbackLatched) st |= status::kCircuitFallback;
    if (cd.resetRequested) st |= status::kResetRequested;
    if (od.fallbackLatched) st |= status::kContainment;
    if (fading_) st |= status::kGraphFading;
    if (incoming_) st |= status::kQualityTransition;
    if (pending_) st |= status::kLatencyChangePending;
    return st;
}

ProcessResult Engine::process(const ProcessBlock& b) noexcept {
    ProcessResult r;
    if (!validateCall(b.ctx, b.buses, r)) return r;
    uint32_t st = 0;
    graphBoundary(*active_, st);
    const uint32_t frames = b.ctx.frames;
    const uint32_t total = b.events ? b.eventCount : 0;
    const uint32_t count = std::min(total, kMaxEventsPerCall);
    uint32_t pos = 0, ev = 0;
    for (;;) {
        // Events in bounded sample order: an offset behind the current position applies now.
        while (ev < count && std::min(b.events[ev].offset, frames) <= pos) applyEverywhere(b.events[ev++]);
        if (pos >= frames) break;
        uint32_t end = frames;
        if (ev < count) end = std::min(end, std::min(b.events[ev].offset, frames));
        end = std::min(end, pos + uint32_t(active_->maxBlock_));
        runSlice(*active_, b.buses, int(pos), int(end - pos), st);   // active_ may change at a fade end
        pos = end;
    }
    if (total > count) {
        // Overflow: latest value per parameter, applied at the end of the call, never blocking.
        std::array<bool, std::size_t(ParamTarget::Count)> seen{};
        for (uint32_t k = total; k-- > count;) {
            const auto t = std::size_t(b.events[k].target);
            if (t >= seen.size() || seen[t]) continue;
            seen[t] = true;
            applyEverywhere(b.events[k]);
        }
        bump(eventsCoalesced_);
        st |= status::kEventsCoalesced;
    }
    r.frames = frames;
    r.status = finishStatus(*active_, st);
    captureBoundary();
    return r;
}

ProcessResult Engine::process(const ProcessContext& ctx, const AudioBuses& buses,
                              std::span<const state::TransactionEvent> events) noexcept {
    ProcessResult r;
    if (!validateCall(ctx, buses, r)) return r;
    uint32_t st = 0;
    graphBoundary(*active_, st);
    const uint32_t frames = ctx.frames;
    const std::size_t count = events.size();
    std::size_t ev = 0;
    uint32_t pos = 0;
    for (;;) {
        while (ev < count && std::min(events[ev].sampleOffset, frames) <= pos) applyEverywhere(events[ev++]);
        if (pos >= frames) break;
        uint32_t end = frames;
        if (ev < count) end = std::min(end, std::min(events[ev].sampleOffset, frames));
        end = std::min(end, pos + uint32_t(active_->maxBlock_));
        runSlice(*active_, buses, int(pos), int(end - pos), st);   // active_ may change at a fade end
        pos = end;
    }
    for (; ev < count; ++ev) applyEverywhere(events[ev]);
    r.frames = frames;
    r.status = finishStatus(*active_, st);
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
    if (incoming_ && !retired_) { retired_ = std::move(incoming_); qWarm_ = qPos_ = 0; }   // abandon the transition
    p.circuit_.reset();
    if (curGraph_) curGraph_->circuit().reset();
    p.output_.reset();
    MacroRuntime& m = p.macro_;
    m.smoother.prepare(p.fs_, m.targets, 4, 0.020);
    m.dyn.prepare(p.fs_, m.dynTargets, 2, 0.020);
    blueprint::TimelineSnapshot ts = m.timeline.snapshot();
    ts.smoothed = ts.target; ts.rampLeft = 0; ts.rampStep = 0;
    m.timeline.restore(ts);
    for (auto& z : m.z) z[0] = z[1] = 0.0;
    for (auto& z : m.lz) z[0] = z[1] = 0.0;
    seedCreative(m, p.fs_, p.cutSeed_, p.delaySeed_, p.noiseSeed_);
    m.transient.reset();
    m.sidechain.reset();
    m.cutOct = m.lpfOct = 0.0;
    m.lpf = designLowpass(p.fs_, p.cutoffHz_);
    m.lastB = m.lastP = m.lastIExt = 0.0;
    for (int c = 0; c < p.channels_; ++c) { p.pitch_[c].reset(); p.pitchDry_[c].reset(); }
    const double v[6] = {m.targets[0], m.targets[1], m.targets[2], m.targets[3], m.timeline.smoothedU(), 0.0};
    evaluateMacros(m, p.bindings_, p.base_, p.fs_, v);
}

// ---------------------------------------------------------------------------------------------
// Live quality transitions

QualityRequest Engine::requestQuality(std::unique_ptr<PreparedEngine>& h) noexcept {
    if (!h || !active_ || incoming_ || retired_ || graphs_ || fading_) { bump(qRejected_); return QualityRequest::Rejected; }
    PreparedEngine& a = *active_;
    PreparedEngine& b = *h;
    if (b.fs_ != a.fs_ || b.channels_ != a.channels_ || b.maxBlock_ != a.maxBlock_ || b.generation_ <= a.generation_) {
        bump(qRejected_);
        return QualityRequest::Rejected;
    }
    const bool live = b.quality_.family == a.quality_.family && b.quality_.zeroLatency == a.quality_.zeroLatency &&
                      b.quality_.latencySamples == a.quality_.latencySamples && b.pitchOn_ == a.pitchOn_;
    if (!live) {
        pending_.swap(h);   // h now holds the previously pending engine (or null) for reclamation
        bump(qStaged_);
        return QualityRequest::Staged;
    }
    incoming_ = std::move(h);
    PreparedEngine& q = *incoming_;
    q.macro_ = a.macro_;                         // same Fs: identical layout and coefficients
    (void)q.circuit_.adoptSlowState(a.circuit_); // temperatures, 1 kHz phase, rail, feedback values
    const OutputParams op = a.output_.params();
    q.output_.setInputGainDb(op.inputGainDb); q.output_.setOutputTrimDb(op.outputTrimDb); q.output_.setMix(op.mix);
    q.output_.setAutoGain(op.autoGain); q.output_.setHpfEnabled(op.hpfEnabled); q.output_.setHpfCutoffHz(op.hpfCutoffHz);
    q.output_.setSoftClipEnabled(op.softClipEnabled); q.output_.setHostBypass(op.hostBypass);
    // Pre-roll covers the full support of the linear-phase FIR chain (about twice its declared group
    // delay) plus a short margin for the circuit's own memory, so the faded-in path is initialized.
    qWarm_ = 2 * q.quality_.latencySamples + kQualityPrerollMargin;
    qPos_ = 0;
    qLen_ = int(std::max(1.0, std::round(kQualityFadeSeconds * q.fs_)));
    bump(qCrossfades_);
    return QualityRequest::Crossfading;
}

bool Engine::takeRetired(std::unique_ptr<PreparedEngine>& out) noexcept {
    if (!retired_ || out) return false;
    out = std::move(retired_);
    return true;
}

QualityStatus Engine::qualityStatus() const noexcept {
    QualityStatus s;
    if (active_) s.active = active_->quality_;
    if (incoming_) s.incoming = incoming_->quality_;
    if (pending_) s.pending = pending_->quality_;
    s.transitioning = incoming_ != nullptr;
    s.fading = incoming_ != nullptr && qWarm_ == 0;
    s.latencyChangePending = pending_ != nullptr;
    s.warmupLeft = qWarm_; s.fadePos = qPos_; s.fadeLen = qLen_;
    s.crossfades = qCrossfades_; s.completed = qCompleted_; s.staged = qStaged_; s.rejected = qRejected_;
    s.activations = qActivations_; s.qualityRequests = qRequests_; s.pitchDelayRequests = pitchRequests_;
    return s;
}

bool Engine::activatePending(std::string& err) {
    if (!pending_) { err = "no pending quality change"; return false; }
    incoming_.reset(); retired_.reset();   // non-audio: abandon/reclaim any transition
    qWarm_ = qPos_ = 0;
    if (active_ && active_->fs_ == pending_->fs_ && active_->channels_ == pending_->channels_) {
        pending_->macro_ = active_->macro_;
        (void)pending_->circuit_.adoptSlowState(active_->circuit_);
    }
    std::unique_ptr<PreparedEngine> h = std::move(pending_);
    if (!tryInstallPrepared(h)) { err = "stale pending generation"; return false; }
    h.reset();   // previous engine reclaimed here (non-audio)
    bump(qActivations_);
    err.clear();
    return true;
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
    if (!active_ || incoming_) return false;
    if (active_->supplyOn_) return false;   // graph processors carry no supply reservoir (documented)
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
    currentCircuit().saveState(w);
    p.output_.saveState(w);
}

bool Engine::readState(StateReader& r, bool apply) noexcept {
    PreparedEngine& p = *active_;
    StateHeader h;
    if (!r.pod(h) || h.magic != kStateMagic || h.channels != p.channels_ || h.maxBlock != p.maxBlock_ || h.samples < 0) return false;
    MacroRuntime m;
    if (!r.pod(m)) return false;
    if (!(m.drive > 0 && std::isfinite(m.drive)) || m.smoother.size() != 4 || m.dyn.size() != 2) return false;
    if (!(std::isfinite(m.noiseGain) && m.noiseGain >= 0 && std::isfinite(m.cutOct) && std::isfinite(m.lpfOct) &&
          m.scSource >= 0 && m.scSource <= 2 && m.scHpfHz >= 20 && m.scHpfHz <= 300))
        return false;
    if (!currentCircuit(p).loadState(r, apply)) return false;
    if (!p.output_.loadState(r, apply)) return false;
    if (apply) {
        samples_ = h.samples; epoch_ = h.epoch; haveEpoch_ = h.haveEpoch != 0;
        p.macro_ = m;
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
    if (!active_ || fading_ || incoming_ || active_->pitchOn_) return 0;
    const std::size_t n = snapshotBytes();
    if (out.size() != n) return 0;
    StateWriter w(out.data(), out.size());
    writeState(w);
    return w.ok() && w.size() == n ? n : 0;
}

bool Engine::loadState(std::span<const std::byte> in) noexcept {
    if (!active_ || incoming_ || active_->pitchOn_ || in.size() != snapshotBytes()) return false;
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
    if (!active_ || !src.active_ || src.fading_ || src.incoming_ || incoming_) return false;
    const std::size_t n = snapshotBytes();
    if (scratch.size() < n || !(src.snapshotIdentity() == snapshotIdentity())) return false;
    if (src.captureState(scratch.first(n)) != n) return false;
    return loadState(std::span<const std::byte>(scratch.data(), n));
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
        d.transientEvidence = m.lastB; d.preservation = m.lastP; d.extCurrentA = m.lastIExt;
        d.preserveAmount = m.dyn.value(0); d.sidechainDepth = m.dyn.value(1);
        d.sidechainSource = LoadSource(m.scSource);
        d.sidechainConnected = m.scConnected != 0;
        d.cutoffMotionQ = m.cutMotion.q(); d.delayMotionQ = m.delayMotion.q();
        d.creativeCutoffHz = p.cutoffOn_ ? p.cutoffHz_ * std::exp2(m.lpfOct) : 0.0;
        d.noiseRms = m.noiseGain;
        d.analogNoise = m.analogNoise != 0;
        d.pitchDelaySamples = p.pitchOn_ ? p.pitch_[0].currentDelaySamples() : 0.0;
    }
    return d;
}

}  // namespace cd::runtime
