// CircuitDrift runtime: A/B dual-engine path (see SlotEngine.h). Owner: cd_runtime.
#include "runtime/SlotEngine.h"
#include <algorithm>

namespace cd::runtime {

SlotEngine::SlotEngine() : mixer_(64) {}

bool SlotEngine::prepare(const EngineConfig& a, const EngineConfig& b, std::string& err) {
    if (a.sampleRate != b.sampleRate || a.channels != b.channels) { err = "A/B slots need identical Fs and channel layout"; return false; }
    EngineConfig ca = a, cb = b;
    ca.slotRole = core::ParamScope::SlotA;
    cb.slotRole = core::ParamScope::SlotB;
    channels_ = 0;   // unusable until every step succeeded
    if (!eng_[0].prepare(ca, err) || !eng_[1].prepare(cb, err)) return false;
    if (eng_[0].latencySamples() != eng_[1].latencySamples()) { err = "A/B slots must declare identical latency"; return false; }
    if (!mixer_.prepare(a.sampleRate)) { err = "A/B mixer rejected sample rate"; return false; }
    for (auto& o : out_) o.assign(std::size_t(a.channels) * kMaxFrames, 0.0);
    weight_.assign(kMaxFrames, 0.0);
    zeros_.assign(kMaxFrames, 0.0);
    ones_.assign(kMaxFrames, 1.0);
    copyScratch_.assign(std::max(eng_[0].snapshotBytes(), eng_[1].snapshotBytes()) + 64, std::byte{0});
    diag_ = {};
    channels_ = a.channels;
    err.clear();
    return true;
}

ProcessResult SlotEngine::process(const ProcessContext& ctx, const AudioBuses& buses,
                                  std::span<const state::TransactionEvent> events) noexcept {
    ProcessResult r;
    if (channels_ == 0 || ctx.frames > kMaxFrames || buses.channels != channels_) {
        if (ctx.frames > kMaxFrames) ++diag_.oversizedCalls;
        if (buses.out && buses.channels >= 1 && buses.channels <= 64)
            for (int c = 0; c < buses.channels; ++c) if (buses.out[c]) std::fill(buses.out[c], buses.out[c] + ctx.frames, 0.0);
        r.status = channels_ == 0 ? status::kNotPrepared : status::kLayoutMismatch;
        return r;
    }
    const state::AbBlockActions act = mixer_.beginBlock();
    if (act.swapped) ++diag_.swaps;
    const int A = mixer_.physicalA(), B = mixer_.physicalB();
    if (act.copyAToB) {
        if (eng_[B].copyStateFrom(eng_[A], std::span<std::byte>(copyScratch_))) ++diag_.copies;
        else ++diag_.copiesRefused;
    }
    eng_[A].setSlotRole(core::ParamScope::SlotA);
    eng_[B].setSlotRole(core::ParamScope::SlotB);
    for (int e = 0; e < 2; ++e) {
        double* o[2] = {};
        for (int c = 0; c < channels_; ++c) o[c] = out_[e].data() + std::size_t(c) * kMaxFrames;
        const AudioBuses eb{channels_, buses.in, o};
        const ProcessResult re = eng_[e].process(ctx, eb, events);
        r.status |= re.status;
        r.frames = re.frames;
    }
    if (ctx.frames == 0) return r;
    if (!buses.out) return r;
    // One weight sequence for every channel (AbMixer advances its ramp once per mix call).
    mixer_.mix(zeros_.data(), ones_.data(), weight_.data(), ctx.frames);
    for (int c = 0; c < channels_; ++c) {
        const double* a = out_[A].data() + std::size_t(c) * kMaxFrames;
        const double* b = out_[B].data() + std::size_t(c) * kMaxFrames;
        double* y = buses.out[c];
        for (uint32_t i = 0; i < ctx.frames; ++i) {
            const double x = weight_[i];
            y[i] = x == 1.0 ? b[i] : a[i] + x * (b[i] - a[i]);   // AbMixer law, exact for identical slots
        }
    }
    return r;
}

ProcessResult SlotEngine::processQueued(const ProcessContext& ctx, const AudioBuses& buses, state::TransactionQueue& q) noexcept {
    return process(ctx, buses, q.drainSlice(ctx.frames));
}

}  // namespace cd::runtime
