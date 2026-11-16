// CircuitDrift edition: LITE processor = the shared cd::runtime::Engine configured by LiteMapping v1
// (07-lite-edition; CD-F05 / T05). Owner: cd_edition.
// Host events: only the two LITE primary controls are accepted. lite.sag (1040) becomes the Sag
// macro event; lite.thermalAge (1041) becomes Age = a and Drift = 0.25*a events at the SAME sample
// offset, so LITE automation is sample-accurate and equivalent to the full engine receiving the
// mapped macro events. Every other host ID (full-only parameters), note and config event is
// rejected and counted (no full-only parameter leakage); rejected events never change audio.
// Threading: prepare() non-audio; process()/processQueued() noexcept, allocation/lock free.
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "edition/LiteState.h"
#include "runtime/Engine.h"
#include "state/TransactionQueue.h"

namespace cd::edition {

class LiteEngine {
public:
    struct Diagnostics { std::int64_t accepted = 0, rejected = 0; };
    bool prepare(const LiteState& s, double fs, int maxBlock, int channels, std::string& error);
    runtime::ProcessResult process(const runtime::ProcessContext& ctx, const runtime::AudioBuses& buses,
                                   std::span<const state::TransactionEvent> events) noexcept;
    runtime::ProcessResult processQueued(const runtime::ProcessContext& ctx, const runtime::AudioBuses& buses,
                                         state::TransactionQueue& queue) noexcept;
    const runtime::Engine& engine() const noexcept { return engine_; }
    int latencySamples() const noexcept { return engine_.latencySamples(); }
    const LiteState& state() const noexcept { return state_; }
    Diagnostics diagnostics() const noexcept { return diag_; }
private:
    runtime::Engine engine_;
    LiteState state_{};
    std::vector<state::TransactionEvent> xlat_;   // 2 * kSliceEventCapacity, preallocated
    Diagnostics diag_{};
};

}  // namespace cd::edition
