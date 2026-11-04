// CircuitDrift runtime: input gain, level-matched blend, output trim, HPF, oversampled soft clip,
// host bypass and emergency containment (CD-F22/CD-F23; 01-runtime "Output protection and
// level-matched blend", "UX additions"). Owner: cd_runtime. Depends only on cd_dsp.
//
// Per host sample: [containment dry fallback on wet] -> linked RMS match (400 ms / 1 s, wet only,
// 50 ms enable ramp) -> y=(1-m)dry+m*g*wet -> output trim -> HPF (filtered and unfiltered paths
// both running, 20 ms enable crossfade, 20 ms log-frequency cutoff ramp) -> soft clip inside its
// own prepared Oversampler stage (enable crossfade 20 ms; latency padded like the circuit) ->
// host-bypass crossfade (10 ms) to untrimmed original dry delayed by the total declared latency ->
// emergency containment (non-finite -> 0 + flag; |y| > 8 -> clamp +-8 + flag). Either containment
// event latches a 10 ms crossfade of the blend's wet input to the latency-aligned dry, held until
// reset(). Gains/mix use 10 ms one-pole smoothing (input gain via applyInputGain()).
// Design choices not fixed by the spec (documented, reversible): soft-clip enable uses a 20 ms
// linear crossfade; auto-gain defaults off; stateful-stage guards reset a stage whose output turns
// non-finite and count it (cannot occur with the Engine entry range guard, kept defensively).
// Soft clip bounds the HIGH-rate signal by 1; decimated sample peaks may overshoot slightly and no
// intersample true-peak ceiling is claimed. Every stage processes one host sample at a time, so the
// result is independent of block partition.
// saveState()/loadState() serialise every dynamic field (smoothers, ramps, matcher, HPF, soft-clip
// oversampler/pad, bypass delay, latch, counters and the current parameter targets) for engine
// snapshots and A/B copy; loadState(apply=false) only validates.
// Threading: prepare() allocates off the audio thread and installs only on success; setters,
// process(), reset(), saveState()/loadState() are noexcept and allocation-free.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include "dsp/Oversampling.h"
#include "runtime/RuntimeCommon.h"

namespace cd::runtime {

struct OutputParams {
    double inputGainDb = 0;     // -24..+24
    double outputTrimDb = 0;    // -24..+24
    double mix = 1;             // 0..1
    bool autoGain = false;
    bool hpfEnabled = true;
    double hpfCutoffHz = 15;    // 10..250
    bool softClipEnabled = true;
    bool hostBypass = false;
};

struct OutputConfig {
    double sampleRate = 48000;
    int maxBlock = 512;
    int channels = 1;
    dsp::Oversampler::Spec clipOs{};   // soft-clip stage rate/family (normally the selected OS)
    bool padToFamilyMax = true;
    int upstreamLatency = 0;           // circuit declared latency (dry/wet arrive aligned to it)
    OutputParams initial{};
};

class OutputStage {
public:
    static constexpr int kMaxChannels = 2;
    static constexpr double kContainLimit = 8.0;

    struct Diagnostics {
        long nonFiniteOutputs = 0, clampedOutputs = 0, stageResets = 0;
        bool fallbackLatched = false;
        double fallbackWeight = 0, bypassWeight = 0, hpfWeight = 0, clipWeight = 0, autoGainWeight = 0;
        double matchGain = 1, hpfCutoffHz = 0, mix = 0, trim = 0, inputGain = 0;
    };

    OutputStage();
    ~OutputStage();
    OutputStage(const OutputStage&) = delete;
    OutputStage& operator=(const OutputStage&) = delete;

    bool prepare(const OutputConfig& cfg, std::string& error);
    bool prepared() const noexcept { return core_ != nullptr; }

    // Parameter setters: values are clamped to their ranges; non-finite values are ignored.
    void setInputGainDb(double db) noexcept;
    void setOutputTrimDb(double db) noexcept;
    void setMix(double m) noexcept;
    void setAutoGain(bool on) noexcept;
    void setHpfEnabled(bool on) noexcept;
    void setHpfCutoffHz(double hz) noexcept;
    void setSoftClipEnabled(bool on) noexcept;
    void setHostBypass(bool on) noexcept;
    OutputParams params() const noexcept;

    // Smoothed input gain, applied before the circuit (dry and wet share it). in may equal out.
    void applyInputGain(const double* const* in, double* const* out, int n) noexcept;
    // rawDry: entry-sanitized input before input gain (host bypass source), delayed internally by
    // upstreamLatency + latencySamples(). dryAligned/wetAligned: circuit outputs, aligned.
    // out may alias any input buffer of the same channel. n in [0, maxBlock].
    void process(const double* const* rawDry, const double* const* dryAligned, const double* const* wetAligned,
                 double* const* out, int n) noexcept;
    void reset() noexcept;
    void saveState(StateWriter& w) const noexcept;
    bool loadState(StateReader& r, bool apply) noexcept;

    int latencySamples() const noexcept;           // own soft-clip stage declared latency
    int totalLatencySamples() const noexcept;      // upstream + own
    const LatencyPlan& latencyPlan() const noexcept;
    Diagnostics diagnostics() const noexcept;

private:
    struct Core;
    std::unique_ptr<Core> core_;
    OutputParams pending_{};
};

}  // namespace cd::runtime
