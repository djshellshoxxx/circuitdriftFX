// CircuitDrift runtime: A/B dual-engine path (CD-F24; 05-state "State ownership and A/B";
// 02-musical "Macro values are global across both A/B paths; blueprint/base condition is per slot").
// Owner: cd_runtime.
// Two complete Engines run continuously on identical input; their outputs are combined by
// cd::state::AbMixer's equal-sum law out = a + x*(b - a) (x = 1 -> exactly b), so identical slots
// reproduce a single engine bit-exactly. Both engines must declare identical latency (checked at
// prepare). Each call drains the TransactionQueue once and gives the same event span to both
// engines: global IDs (macros, gains, mix, HPF, soft clip) reach both; slot.a.* IDs only the
// engine currently acting as A, slot.b.* only B (roles follow AbMixer swaps). Blueprint graphs
// are per slot: attach a GraphInstaller to engine(i) individually (<= 2 graph processors per
// engine, <= 4 in total). ab.crossfade (1017) is a host parameter whose writer is the message-
// thread AbMixer command API (mixer().setPosition); on the audio queue it is counted unsupported.
// Copy A->B (AbMixer::copyAToB) copies the physical A engine's dynamic state and parameter targets
// into B at the block boundary when both have identical snapshot identities (same graph/topology);
// otherwise it is refused and counted (configuration copy across topologies is a worker job).
// Threading: prepare() non-audio; process() noexcept, allocation/lock free; mixer() commands from
// one message thread.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "runtime/Engine.h"
#include "state/AbSlots.h"

namespace cd::runtime {

class SlotEngine {
public:
    static constexpr uint32_t kMaxFrames = 8192;
    struct Diagnostics { int64_t swaps = 0, copies = 0, copiesRefused = 0, oversizedCalls = 0; };

    SlotEngine();
    // Non-audio. Prepares physical engine 0 from a and 1 from b. Rejects different Fs/channels or
    // declared latency (A/B and dry paths must share one declared delay).
    bool prepare(const EngineConfig& a, const EngineConfig& b, std::string& error);
    ProcessResult process(const ProcessContext& ctx, const AudioBuses& buses,
                          std::span<const state::TransactionEvent> events) noexcept;
    ProcessResult processQueued(const ProcessContext& ctx, const AudioBuses& buses, state::TransactionQueue& queue) noexcept;

    Engine& engine(int physical) noexcept { return eng_[physical & 1]; }
    const Engine& engine(int physical) const noexcept { return eng_[physical & 1]; }
    state::AbMixer& mixer() noexcept { return mixer_; }
    int latencySamples() const noexcept { return eng_[0].latencySamples(); }
    Diagnostics diagnostics() const noexcept { return diag_; }

private:
    Engine eng_[2];
    state::AbMixer mixer_;
    int channels_ = 0;
    std::vector<double> out_[2];             // per engine: channels * kMaxFrames
    std::vector<double> weight_, zeros_, ones_;
    std::vector<std::byte> copyScratch_;
    Diagnostics diag_{};
};

}  // namespace cd::runtime
