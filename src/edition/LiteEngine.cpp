// CircuitDrift edition: LITE processor (see LiteEngine.h). Owner: cd_edition.
#include "edition/LiteEngine.h"
#include <algorithm>
#include "core/ParameterRegistry.h"

namespace cd::edition {

bool LiteEngine::prepare(const LiteState& s, double fs, int maxBlock, int channels, std::string& err) {
    PreparedEngineConfig cfg;
    if (!liteEngineConfig(s.knobs, fs, maxBlock, channels, cfg, err)) return false;
    if (!engine_.prepare(cfg.engine, err)) return false;   // circuit deep-copies the graph's manifests
    state_ = s;
    xlat_.assign(2 * state::kSliceEventCapacity, state::TransactionEvent{});
    diag_ = {};
    return true;
}

runtime::ProcessResult LiteEngine::process(const runtime::ProcessContext& ctx, const runtime::AudioBuses& buses,
                                           std::span<const state::TransactionEvent> events) noexcept {
    std::size_t n = 0;
    const std::size_t cap = xlat_.size();
    for (const state::TransactionEvent& e : events) {
        if (e.kind != state::EventKind::Param || (e.hostId != kLiteSagHostId && e.hostId != kLiteThermalAgeHostId) || n + 2 > cap) {
            ++diag_.rejected;
            continue;
        }
        const double v = core::toPlain(*core::findByHostId(e.hostId), e.normalized);   // linear [0,1]
        ++diag_.accepted;
        if (e.hostId == kLiteSagHostId) {
            xlat_[n++] = state::TransactionEvent::param(1001, v, e.sampleOffset);                        // macro.sag = s
        } else {
            xlat_[n++] = state::TransactionEvent::param(1000, v, e.sampleOffset);                        // macro.age = a
            xlat_[n++] = state::TransactionEvent::param(1002, kLiteDriftPerAge * v, e.sampleOffset);     // macro.drift = 0.25 a
        }
    }
    return engine_.process(ctx, buses, std::span<const state::TransactionEvent>(xlat_.data(), n));
}

runtime::ProcessResult LiteEngine::processQueued(const runtime::ProcessContext& ctx, const runtime::AudioBuses& buses,
                                                 state::TransactionQueue& q) noexcept {
    return process(ctx, buses, q.drainSlice(ctx.frames));
}

}  // namespace cd::edition
