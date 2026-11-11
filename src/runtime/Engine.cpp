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
                   kSlotBLifecycle = 1020, kSlotBScenario = 1021;

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
                    const double* v) noexcept {
    const blueprint::Condition cond = m.timeline.condition();
    control::TimelineOffsets tl;
    tl.serviceAgeYears = cond.values.ageYears;
    tl.tolSigma = cond.values.toleranceSigma;
    tl.leakFrac = cond.values.leakageFraction;
    tl.driveDb = 0.0;
    tl.biasOffset = cond.values.biasOffsetSpan * mb.biasSpan;
    control::Hooks hooks;
    hooks.timeline = &tl;
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

}  // namespace

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
    OutputConfig oc;
    oc.sampleRate = cfg.sampleRate; oc.maxBlock = cfg.maxBlock; oc.channels = cfg.channels;
    oc.clipOs = spec; oc.padToFamilyMax = !cfg.quality.zeroLatency;
    oc.upstreamLatency = p->circuit_.latencySamples(); oc.initial = cfg.initial;
    if (!p->output_.prepare(oc, err)) return nullptr;
    p->quality_.circuitLatency = p->circuit_.latencySamples();
    p->quality_.clipLatency = p->output_.latencySamples();
    p->quality_.latencySamples = p->output_.totalLatencySamples();
    p->os_ = spec; p->pad_ = !cfg.quality.zeroLatency;
    p->fs_ = cfg.sampleRate; p->channels_ = cfg.channels; p->maxBlock_ = cfg.maxBlock;
    const std::size_t sz = std::size_t(cfg.channels) * std::size_t(cfg.maxBlock);
    p->raw_.assign(sz, 0.0); p->gained_.assign(sz, 0.0); p->wet_.assign(sz, 0.0); p->dry_.assign(sz, 0.0);
    p->wet2_.assign(sz, 0.0); p->dry2_.assign(sz, 0.0);
    p->drive_.assign(std::size_t(cfg.maxBlock), 1.0);
    p->shelf_.assign(std::size_t(cfg.maxBlock) * 5, 0.0);
    p->bindings_ = cfg.macroBindings;
    p->base_ = cfg.base;
    initMacros(p->macro_, cfg, p->bindings_, cfg.sampleRate);
    p->fadeLen_ = int(std::max(1.0, std::round(kGraphFadeSeconds * cfg.sampleRate)));
    Fnv64 h;
    h.str("cd.runtime.Engine/1");
    h.bytes(state::kEngineModelVersion.data(), state::kEngineModelVersion.size());
    h.pod(cfg.channels); h.pod(p->quality_.factor); h.pod(p->quality_.family); h.pod(p->quality_.maxNewton);
    h.pod(p->quality_.zeroLatency); h.pod(p->quality_.latencySamples);
    hashBindings(h, p->bindings_);
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
    OutputStage& o = active_->output_;
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

void Engine::applyHostParam(PreparedEngine& p, uint32_t hostId, double normalized) noexcept {
    const core::ParamSpec* spec = core::findByHostId(hostId);
    if (!spec) { bump(unknown_); return; }
    if (spec->scope != core::ParamScope::Global && spec->scope != role_) { bump(otherSlot_); return; }
    bump(paramEvents_);
    const double v = core::toPlain(*spec, normalized);
    const bool on = v >= 0.5;
    OutputStage& o = p.output_;
    MacroRuntime& m = p.macro_;
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

void Engine::macroSlice(PreparedEngine& p, int n) noexcept {
    MacroRuntime& m = p.macro_;
    const std::size_t M = std::size_t(p.maxBlock_);
    double* b0 = p.shelf_.data(); double* b1 = b0 + M; double* b2 = b1 + M; double* a1 = b2 + M; double* a2 = a1 + M;
    for (int i = 0; i < n; ++i) {
        m.smoother.step();
        const double u = m.timeline.tick();
        const double v[5] = {m.smoother.value(0), m.smoother.value(1), m.smoother.value(2), m.smoother.value(3), u};
        if (v[0] != m.last[0] || v[1] != m.last[1] || v[2] != m.last[2] || v[3] != m.last[3] || v[4] != m.last[4])
            evaluateMacros(m, p.bindings_, p.base_, p.fs_, v);
        p.drive_[std::size_t(i)] = m.drive;
        b0[i] = m.shelf.b0; b1[i] = m.shelf.b1; b2[i] = m.shelf.b2; a1[i] = m.shelf.a1; a2[i] = m.shelf.a2;
    }
}

void Engine::runSlice(PreparedEngine& p, const double* const* in, double* const* out, int off, int n, uint32_t& st) noexcept {
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
    macroSlice(p, n);
    const double* drive = p.drive_.data();
    CircuitRuntime& cur = currentCircuit(p);
    if (!fading_) {
        cur.process(gainedC, wet, dry, n, drive);
    } else {
        CircuitRuntime& old = oldGraph_ ? oldGraph_->circuit() : p.circuit_;
        old.process(gainedC, wet2, dry2, n, drive);
        cur.process(gainedC, wet, dry, n, drive);
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
    return st;
}

ProcessResult Engine::process(const ProcessBlock& b) noexcept {
    ProcessResult r;
    if (!validateCall(b.ctx, b.buses, r)) return r;
    PreparedEngine* p = active_.get();
    uint32_t st = 0;
    graphBoundary(*p, st);
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
        end = std::min(end, pos + uint32_t(p->maxBlock_));
        runSlice(*p, b.buses.in, b.buses.out, int(pos), int(end - pos), st);
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
    const uint32_t frames = ctx.frames;
    const std::size_t count = events.size();
    std::size_t ev = 0;
    uint32_t pos = 0;
    for (;;) {
        while (ev < count && std::min(events[ev].sampleOffset, frames) <= pos) applyTransaction(*p, events[ev++]);
        if (pos >= frames) break;
        uint32_t end = frames;
        if (ev < count) end = std::min(end, std::min(events[ev].sampleOffset, frames));
        end = std::min(end, pos + uint32_t(p->maxBlock_));
        runSlice(*p, buses.in, buses.out, int(pos), int(end - pos), st);
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
    const double v[5] = {m.targets[0], m.targets[1], m.targets[2], m.targets[3], m.timeline.smoothedU()};
    evaluateMacros(m, p.bindings_, p.base_, p.fs_, v);
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
    currentCircuit().saveState(w);
    p.output_.saveState(w);
}

bool Engine::readState(StateReader& r, bool apply) noexcept {
    PreparedEngine& p = *active_;
    StateHeader h;
    if (!r.pod(h) || h.magic != kStateMagic || h.channels != p.channels_ || h.maxBlock != p.maxBlock_ || h.samples < 0) return false;
    MacroRuntime m;
    if (!r.pod(m)) return false;
    if (!(m.drive > 0 && std::isfinite(m.drive)) || m.smoother.size() != 4) return false;
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
    }
    return d;
}

}  // namespace cd::runtime
