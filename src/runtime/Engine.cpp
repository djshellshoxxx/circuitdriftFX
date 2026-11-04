// CircuitDrift runtime: integrated engine implementation (see Engine.h). Owner: cd_runtime.
#include "runtime/Engine.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace cd::runtime {
namespace {

void bump(long& c) noexcept { if (c < std::numeric_limits<long>::max()) ++c; }

void silence(const ProcessBlock& b) noexcept {
    if (!b.buses.out || b.buses.channels < 1 || b.buses.channels > 64) return;
    for (int c = 0; c < b.buses.channels; ++c)
        if (b.buses.out[c]) std::fill(b.buses.out[c], b.buses.out[c] + b.ctx.frames, 0.0);
}

}  // namespace

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
    p->fs_ = cfg.sampleRate; p->channels_ = cfg.channels; p->maxBlock_ = cfg.maxBlock;
    const std::size_t sz = std::size_t(cfg.channels) * std::size_t(cfg.maxBlock);
    p->raw_.assign(sz, 0.0); p->gained_.assign(sz, 0.0); p->wet_.assign(sz, 0.0); p->dry_.assign(sz, 0.0);
    p->generation_ = ++nextGeneration_;
    err.clear();
    return p;
}

bool Engine::tryInstallPrepared(std::unique_ptr<PreparedEngine>& h) noexcept {
    if (!h) return false;
    if (active_ && h->generation_ <= active_->generation_) { bump(staleInstalls_); return false; }
    active_.swap(h);  // previous engine (or null) now in h: caller reclaims off-audio
    return true;
}

bool Engine::prepare(const EngineConfig& cfg, std::string& err) {
    auto p = buildPrepared(cfg, err);
    if (!p) return false;
    if (!tryInstallPrepared(p)) { err = "stale generation"; return false; }
    p.reset();  // reclaim previous engine here (non-audio caller)
    return true;
}

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

void Engine::runSlice(PreparedEngine& p, const double* const* in, double* const* out, int off, int n, uint32_t& st) noexcept {
    const int C = p.channels_;
    const std::size_t M = std::size_t(p.maxBlock_);
    const double* rawC[2] = {};
    double* raw[2] = {}; double* gained[2] = {}; double* wet[2] = {}; double* dry[2] = {}; double* o[2] = {};
    const double *gainedC[2] = {}, *wetC[2] = {}, *dryC[2] = {};
    for (int c = 0; c < C; ++c) {
        raw[c] = p.raw_.data() + std::size_t(c) * M; gained[c] = p.gained_.data() + std::size_t(c) * M;
        wet[c] = p.wet_.data() + std::size_t(c) * M; dry[c] = p.dry_.data() + std::size_t(c) * M;
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
    p.circuit_.process(gainedC, wet, dry, n);
    p.output_.process(rawC, dryC, wetC, o, n);
    samples_ += n;
}

ProcessResult Engine::process(const ProcessBlock& b) noexcept {
    ProcessResult r;
    PreparedEngine* p = active_.get();
    if (!p) { silence(b); r.status = status::kNotPrepared; return r; }
    if (b.ctx.sampleRate != p->fs_) { silence(b); r.status = status::kRateMismatch; return r; }
    const uint32_t frames = b.ctx.frames;
    bool layoutOk = b.buses.channels == p->channels_;
    if (layoutOk && frames > 0) {
        layoutOk = b.buses.in && b.buses.out;
        for (int c = 0; layoutOk && c < p->channels_; ++c) layoutOk = b.buses.in[c] && b.buses.out[c];
    }
    if (!layoutOk) { silence(b); r.status = status::kLayoutMismatch; return r; }
    if (haveEpoch_ && b.ctx.epoch != epoch_) bump(epochChanges_);
    epoch_ = b.ctx.epoch; haveEpoch_ = true;

    uint32_t st = 0;
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
    const auto cd = p->circuit_.diagnostics();
    const auto od = p->output_.diagnostics();
    if (cd.fallbackLatched) st |= status::kCircuitFallback;
    if (cd.resetRequested) st |= status::kResetRequested;
    if (od.fallbackLatched) st |= status::kContainment;
    r.frames = frames;
    r.status = st;
    return r;
}

void Engine::reset() noexcept {
    if (!active_) return;
    active_->circuit_.reset();
    active_->output_.reset();
}

EngineDiagnostics Engine::diagnostics() const noexcept {
    EngineDiagnostics d;
    d.samples = samples_; d.epoch = epoch_; d.epochChanges = epochChanges_;
    d.inputNonFinite = inputNonFinite_; d.inputClamped = inputClamped_; d.eventsCoalesced = eventsCoalesced_;
    d.staleInstalls = staleInstalls_;
    if (active_) {
        d.generation = active_->generation_;
        d.circuit = active_->circuit_.diagnostics();
        d.output = active_->output_.diagnostics();
    }
    return d;
}

}  // namespace cd::runtime
