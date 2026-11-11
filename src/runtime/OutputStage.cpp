// CircuitDrift runtime: output stage implementation (see OutputStage.h). Owner: cd_runtime.
#include "runtime/OutputStage.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>
#include <vector>
#include "dsp/OutputProtection.h"

namespace cd::runtime {
namespace {

constexpr double kGainTau = 0.010, kAutoGainRamp = 0.050, kHpfRamp = 0.020, kClipRamp = 0.020;
constexpr double kBypassRamp = 0.010, kFallbackRamp = 0.010;

double dbToGain(double db) noexcept { return std::pow(10.0, db / 20.0); }
double clampDb(double db) noexcept { return std::clamp(db, -24.0, 24.0); }
double clampFc(double hz) noexcept { return std::clamp(hz, 10.0, 250.0); }
int rampSamples(double fs, double sec) noexcept { return int(std::max(1.0, std::round(fs * sec))); }
void bump(std::int64_t& c) noexcept { if (c < std::numeric_limits<std::int64_t>::max()) ++c; }

}  // namespace

struct OutputStage::Core {
    struct Channel {
        dsp::ButterworthHpf hpf;
        dsp::Oversampler os;
        DelayLine pad, raw;
        std::vector<double> hi;
        // Soft-clip bypass support (factor > 1): history of the pre-clip signal, L = 2*declared+4
        // samples, used both as the pure-delay path while the clip is fully off and to re-prime the
        // oversampler/pad exactly when it is re-enabled (their memory is shorter than L).
        std::vector<double> hist;
        int histPos = 0;
        bool warm = true;   // oversampler/pad state is current (false while the pure-delay path runs)
    };
    Channel ch[kMaxChannels];
    LatencyPlan plan;
    OnePole inGain, trim, mix;
    LinearRamp autoGain, hpfMix, clipMix, bypass, fallback, logFc;
    dsp::GainMatcher matcher;
    double fs = 0, fcTarget = 15, fcNow = 15;
    int channels = 0, maxBlock = 0, factor = 1, upstream = 0;
    bool latched = false;
    std::int64_t nonFinite = 0, clamped = 0, stageResets = 0;
    mutable std::vector<double> scratch;   // snapshot staging
};

OutputStage::OutputStage() = default;
OutputStage::~OutputStage() = default;

bool OutputStage::prepare(const OutputConfig& cfg, std::string& err) {
    auto bad = [&](const char* why) { err = why; return false; };
    if (!(cfg.sampleRate >= 44100.0 && cfg.sampleRate <= 192000.0)) return bad("sample rate outside 44.1-192 kHz");
    if (cfg.maxBlock < 1 || cfg.maxBlock > dsp::Oversampler::kMaxBlockSize) return bad("maxBlock outside 1..8192");
    if (cfg.channels < 1 || cfg.channels > kMaxChannels) return bad("channels must be 1 or 2");
    if (cfg.upstreamLatency < 0 || cfg.upstreamLatency > 1 << 20) return bad("upstream latency out of range");
    if (cfg.clipOs.factor > 16) return bad("soft-clip stage limited to 1..16x");
    auto core = std::make_unique<Core>();
    if (!planLatency(cfg.clipOs, cfg.padToFamilyMax, core->plan)) return bad("soft-clip oversampler design failed");
    core->fs = cfg.sampleRate; core->channels = cfg.channels; core->maxBlock = cfg.maxBlock;
    core->factor = cfg.clipOs.factor; core->upstream = cfg.upstreamLatency;
    const OutputParams p = cfg.initial;
    core->fcTarget = core->fcNow = clampFc(std::isfinite(p.hpfCutoffHz) ? p.hpfCutoffHz : 15.0);
    for (int c = 0; c < cfg.channels; ++c) {
        auto& ch = core->ch[c];
        if (!ch.hpf.design(cfg.sampleRate, core->fcNow)) return bad("HPF design failed");
        if (!ch.os.prepare(cfg.clipOs, 1)) return bad("soft-clip oversampler prepare failed");
        ch.pad.prepare(core->plan.padHigh);
        ch.raw.prepare(cfg.upstreamLatency + core->plan.declared);
        ch.hi.assign(std::size_t(cfg.clipOs.factor), 0.0);
        ch.hist.assign(std::size_t(2 * core->plan.declared + 4), 0.0);
        ch.histPos = 0;
        ch.warm = true;
    }
    const double fs = cfg.sampleRate;
    core->inGain.prepare(fs, kGainTau); core->trim.prepare(fs, kGainTau); core->mix.prepare(fs, kGainTau);
    core->autoGain.prepare(rampSamples(fs, kAutoGainRamp));
    core->hpfMix.prepare(rampSamples(fs, kHpfRamp));
    core->clipMix.prepare(rampSamples(fs, kClipRamp));
    core->bypass.prepare(rampSamples(fs, kBypassRamp));
    core->fallback.prepare(rampSamples(fs, kFallbackRamp));
    core->logFc.prepare(rampSamples(fs, kHpfRamp));
    core->matcher.prepare(fs);
    core->scratch.assign(std::size_t(core->ch[0].os.stateSize()) + 1, 0.0);
    core_ = std::move(core);
    pending_ = OutputParams{};
    pending_.hpfCutoffHz = core_->fcTarget;
    // Route the initial values through the clamping setters, then snap (no ramp at start).
    setInputGainDb(p.inputGainDb); setOutputTrimDb(p.outputTrimDb); setMix(p.mix);
    setAutoGain(p.autoGain); setHpfEnabled(p.hpfEnabled); setSoftClipEnabled(p.softClipEnabled);
    setHostBypass(p.hostBypass);
    reset();
    err.clear();
    return true;
}

void OutputStage::setInputGainDb(double db) noexcept {
    if (!std::isfinite(db)) return;
    pending_.inputGainDb = clampDb(db);
    if (core_) core_->inGain.setTarget(dbToGain(pending_.inputGainDb));
}
void OutputStage::setOutputTrimDb(double db) noexcept {
    if (!std::isfinite(db)) return;
    pending_.outputTrimDb = clampDb(db);
    if (core_) core_->trim.setTarget(dbToGain(pending_.outputTrimDb));
}
void OutputStage::setMix(double m) noexcept {
    if (!std::isfinite(m)) return;
    pending_.mix = std::clamp(m, 0.0, 1.0);
    if (core_) core_->mix.setTarget(pending_.mix);
}
void OutputStage::setAutoGain(bool on) noexcept {
    pending_.autoGain = on;
    if (core_) core_->autoGain.setTarget(on ? 1.0 : 0.0);
}
void OutputStage::setHpfEnabled(bool on) noexcept {
    pending_.hpfEnabled = on;
    if (core_) core_->hpfMix.setTarget(on ? 1.0 : 0.0);
}
void OutputStage::setHpfCutoffHz(double hz) noexcept {
    if (!std::isfinite(hz)) return;
    pending_.hpfCutoffHz = clampFc(hz);
    if (!core_ || pending_.hpfCutoffHz == core_->fcTarget) return;
    core_->fcTarget = pending_.hpfCutoffHz;
    core_->logFc.snap(std::log(core_->fcNow));
    core_->logFc.setTarget(std::log(core_->fcTarget));
}
void OutputStage::setSoftClipEnabled(bool on) noexcept {
    pending_.softClipEnabled = on;
    if (core_) core_->clipMix.setTarget(on ? 1.0 : 0.0);
}
void OutputStage::setHostBypass(bool on) noexcept {
    pending_.hostBypass = on;
    if (core_) core_->bypass.setTarget(on ? 1.0 : 0.0);
}
OutputParams OutputStage::params() const noexcept { return pending_; }

void OutputStage::applyInputGain(const double* const* in, double* const* out, int n) noexcept {
    Core* k = core_.get();
    if (!k || n <= 0 || n > k->maxBlock || !in || !out) return;
    for (int c = 0; c < k->channels; ++c) if (!in[c] || !out[c]) return;
    for (int i = 0; i < n; ++i) {
        const double g = k->inGain.next();
        for (int c = 0; c < k->channels; ++c) out[c][i] = in[c][i] * g;
    }
}

// Replays the stored pre-clip history through the (identity-clipped) up/pad/down chain so the
// oversampler and pad hold exactly the state an always-running stage would have (FIR memory is
// shorter than the ring, so the result is bit-identical; IIR histories converge). Bounded work.
void OutputStage::primeClipStage(Core& core, int c) noexcept {
    auto& ch = core.ch[c];
    const int F = core.factor;
    const int L = int(ch.hist.size());
    double sink = 0;
    for (int j = 0; j < L; ++j) {
        double v = ch.hist[std::size_t((ch.histPos + j) % L)];
        ch.os.up(&v, ch.hi.data(), 1);
        for (int s = 0; s < F; ++s) ch.hi[std::size_t(s)] = ch.pad.process(ch.hi[std::size_t(s)]);
        ch.os.down(ch.hi.data(), &sink, 1);
    }
}

void OutputStage::process(const double* const* rawDry, const double* const* dryA, const double* const* wetA,
                          double* const* out, int n) noexcept {
    Core* k = core_.get();
    if (!k || n <= 0 || n > k->maxBlock || !rawDry || !dryA || !wetA || !out) return;
    for (int c = 0; c < k->channels; ++c) if (!rawDry[c] || !dryA[c] || !wetA[c] || !out[c]) return;
    const int C = k->channels, F = k->factor;
    const LatencyPlan& plan = k->plan;
    for (int i = 0; i < n; ++i) {
        const double fb = k->fallback.next();
        const double m = k->mix.next(), trim = k->trim.next(), e = k->autoGain.next();
        const double hx = k->hpfMix.next(), cx = k->clipMix.next(), bx = k->bypass.next();
        if (k->logFc.ramping()) {
            const double v = k->logFc.next();
            k->fcNow = k->logFc.ramping() ? std::exp(v) : k->fcTarget;
            for (int c = 0; c < C; ++c) k->ch[c].hpf.design(k->fs, k->fcNow);
        }
        double d[kMaxChannels], w[kMaxChannels], raw[kMaxChannels];
        double pd = 0, pw = 0;
        for (int c = 0; c < C; ++c) {
            raw[c] = k->ch[c].raw.process(rawDry[c][i]);  // read before out (may alias) is written
            d[c] = dryA[c][i];
            w[c] = fb == 0.0 ? wetA[c][i] : (1.0 - fb) * wetA[c][i] + fb * d[c];
            pd += d[c] * d[c]; pw += w[c] * w[c];
        }
        const double g = k->matcher.updateMeanSquares(pd / C, pw / C);
        const double gEff = e == 0.0 ? 1.0 : (e == 1.0 ? g : std::exp(e * std::log(g)));
        bool trip = false;
        for (int c = 0; c < C; ++c) {
            auto& ch = k->ch[c];
            double y = ((1.0 - m) * d[c] + m * gEff * w[c]) * trim;
            double f = ch.hpf.process(y);
            if (!isFiniteSample(f) || !isFiniteSample(ch.hpf.z1) || !isFiniteSample(ch.hpf.z2)) {
                ch.hpf.reset(); f = 0.0; bump(k->stageResets);
            }
            y = hx == 0.0 ? y : (hx == 1.0 ? f : (1.0 - hx) * y + hx * f);
            if (F > 1) {
                // Soft clip fully off (cx == 0): a pure integer delay of the declared latency replaces
                // the up/pad/down round trip (no resampling ripple/band-limit; latency unchanged). The
                // oversampler is re-primed from the history ring when the clip turns back on.
                const int L = int(ch.hist.size());
                const double delayed = ch.hist[std::size_t((ch.histPos + L - plan.declared) % L)];
                if (cx == 0.0) {
                    ch.warm = false;
                    ch.hist[std::size_t(ch.histPos)] = y;
                    ch.histPos = ch.histPos + 1 == L ? 0 : ch.histPos + 1;
                    y = delayed;
                } else {
                    if (!ch.warm) { primeClipStage(*k, c); ch.warm = true; }
                    ch.hist[std::size_t(ch.histPos)] = y;
                    ch.histPos = ch.histPos + 1 == L ? 0 : ch.histPos + 1;
                    ch.os.up(&y, ch.hi.data(), 1);
                    for (int s = 0; s < F; ++s) {
                        const double x = ch.pad.process(ch.hi[std::size_t(s)]);
                        ch.hi[std::size_t(s)] = cx == 1.0 ? dsp::softClip(x) : (1.0 - cx) * x + cx * dsp::softClip(x);
                    }
                    ch.os.down(ch.hi.data(), &y, 1);
                    if (!isFiniteSample(y)) { ch.os.reset(); ch.pad.reset(); bump(k->stageResets); }
                }
            } else {
                ch.os.up(&y, ch.hi.data(), 1);
                for (int s = 0; s < F; ++s) {
                    const double x = ch.pad.process(ch.hi[std::size_t(s)]);
                    ch.hi[std::size_t(s)] = cx == 0.0 ? x : (cx == 1.0 ? dsp::softClip(x) : (1.0 - cx) * x + cx * dsp::softClip(x));
                }
                ch.os.down(ch.hi.data(), &y, 1);
                if (!isFiniteSample(y)) { ch.os.reset(); ch.pad.reset(); bump(k->stageResets); }
            }
            y = bx == 0.0 ? y : (bx == 1.0 ? raw[c] : (1.0 - bx) * y + bx * raw[c]);
            // Emergency containment (independent of the soft clip).
            if (!isFiniteSample(y)) { y = 0.0; bump(k->nonFinite); trip = true; }
            else if (std::fabs(y) > kContainLimit) { y = std::copysign(kContainLimit, y); bump(k->clamped); trip = true; }
            out[c][i] = y;
        }
        if (trip && !k->latched) { k->latched = true; k->fallback.setTarget(1.0); }
    }
}

void OutputStage::reset() noexcept {
    Core* k = core_.get();
    if (!k) return;
    for (int c = 0; c < k->channels; ++c) {
        auto& ch = k->ch[c];
        ch.hpf.reset(); ch.os.reset(); ch.pad.reset(); ch.raw.reset();
        std::fill(ch.hist.begin(), ch.hist.end(), 0.0);
        ch.histPos = 0;
        ch.warm = true;   // zero history and zero oversampler state agree
    }
    const OutputParams& p = pending_;
    k->inGain.snap(dbToGain(p.inputGainDb)); k->trim.snap(dbToGain(p.outputTrimDb)); k->mix.snap(p.mix);
    k->autoGain.snap(p.autoGain ? 1.0 : 0.0); k->hpfMix.snap(p.hpfEnabled ? 1.0 : 0.0);
    k->clipMix.snap(p.softClipEnabled ? 1.0 : 0.0); k->bypass.snap(p.hostBypass ? 1.0 : 0.0);
    k->fallback.snap(0.0);
    k->fcTarget = k->fcNow = p.hpfCutoffHz;
    k->logFc.snap(std::log(k->fcNow));
    for (int c = 0; c < k->channels; ++c) k->ch[c].hpf.design(k->fs, k->fcNow);
    k->matcher.reset();
    k->latched = false;
}

namespace {
// Trivially copyable scalar state of the stage (copied as one item).
struct ScalarState {
    OnePole inGain, trim, mix;
    LinearRamp autoGain, hpfMix, clipMix, bypass, fallback, logFc;
    dsp::GainMatcher matcher;
    OutputParams pending;
    double fcTarget, fcNow;
    std::int64_t latched, nonFinite, clamped, stageResets;
};
static_assert(std::is_trivially_copyable_v<ScalarState>);
static_assert(std::is_trivially_copyable_v<dsp::ButterworthHpf>);
}  // namespace

void OutputStage::saveState(StateWriter& w) const noexcept {
    const Core* k = core_.get();
    if (!k) return;
    const ScalarState sc{k->inGain, k->trim, k->mix, k->autoGain, k->hpfMix, k->clipMix, k->bypass, k->fallback, k->logFc,
                         k->matcher, pending_, k->fcTarget, k->fcNow, k->latched ? 1 : 0, k->nonFinite, k->clamped, k->stageResets};
    w.pod(sc);
    for (int c = 0; c < k->channels; ++c) {
        const auto& ch = k->ch[c];
        w.pod(ch.hpf);
        ch.os.getState(k->scratch.data());
        w.doubles(k->scratch.data(), std::size_t(ch.os.stateSize()));
        ch.pad.save(w);
        ch.raw.save(w);
        const std::int64_t hdr[2] = {ch.histPos, ch.warm ? 1 : 0};
        w.pod(hdr);
        w.doubles(ch.hist.data(), ch.hist.size());
    }
}

bool OutputStage::loadState(StateReader& r, bool apply) noexcept {
    Core* k = core_.get();
    if (!k) return false;
    ScalarState sc{};
    if (!r.pod(sc)) return false;
    if (!(sc.fcNow >= 10 && sc.fcNow <= 250 && sc.fcTarget >= 10 && sc.fcTarget <= 250)) return false;
    if (apply) {
        k->inGain = sc.inGain; k->trim = sc.trim; k->mix = sc.mix; k->autoGain = sc.autoGain; k->hpfMix = sc.hpfMix;
        k->clipMix = sc.clipMix; k->bypass = sc.bypass; k->fallback = sc.fallback; k->logFc = sc.logFc; k->matcher = sc.matcher;
        pending_ = sc.pending; k->fcTarget = sc.fcTarget; k->fcNow = sc.fcNow; k->latched = sc.latched != 0;
        k->nonFinite = std::int64_t(sc.nonFinite); k->clamped = std::int64_t(sc.clamped); k->stageResets = std::int64_t(sc.stageResets);
    }
    for (int c = 0; c < k->channels; ++c) {
        auto& ch = k->ch[c];
        dsp::ButterworthHpf hpf;
        if (!r.pod(hpf)) return false;
        if (apply) ch.hpf = hpf;
        if (!r.doubles(k->scratch.data(), std::size_t(ch.os.stateSize()))) return false;
        if (!(apply ? ch.os.setState(k->scratch.data()) : ch.os.stateValid(k->scratch.data()))) return false;
        if (!ch.pad.load(r, apply) || !ch.raw.load(r, apply)) return false;
        std::int64_t hdr[2] = {0, 0};
        if (!r.pod(hdr) || hdr[0] < 0 || hdr[0] >= std::int64_t(ch.hist.size()) || (hdr[1] != 0 && hdr[1] != 1)) return false;
        if (!(apply ? r.doubles(ch.hist.data(), ch.hist.size()) : r.bytes(nullptr, ch.hist.size() * sizeof(double)))) return false;
        if (apply) { ch.histPos = int(hdr[0]); ch.warm = hdr[1] == 1; }
    }
    return true;
}

int OutputStage::latencySamples() const noexcept { return core_ ? core_->plan.declared : 0; }
int OutputStage::totalLatencySamples() const noexcept { return core_ ? core_->plan.declared + core_->upstream : 0; }
const LatencyPlan& OutputStage::latencyPlan() const noexcept {
    static const LatencyPlan kNone{};
    return core_ ? core_->plan : kNone;
}

OutputStage::Diagnostics OutputStage::diagnostics() const noexcept {
    Diagnostics d;
    if (!core_) return d;
    const Core& k = *core_;
    d.nonFiniteOutputs = k.nonFinite; d.clampedOutputs = k.clamped; d.stageResets = k.stageResets;
    d.fallbackLatched = k.latched; d.fallbackWeight = k.fallback.value(); d.bypassWeight = k.bypass.value();
    d.hpfWeight = k.hpfMix.value(); d.clipWeight = k.clipMix.value(); d.autoGainWeight = k.autoGain.value();
    d.matchGain = k.matcher.gain(); d.hpfCutoffHz = k.fcNow; d.mix = k.mix.value(); d.trim = k.trim.value();
    d.inputGain = k.inGain.value();
    return d;
}

}  // namespace cd::runtime
