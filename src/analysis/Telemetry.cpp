// CircuitDrift analysis: telemetry writer/reader implementation. See Telemetry.h.
#include "analysis/Telemetry.h"

namespace cd::analysis {

bool TelemetryWriter::publish(TelemetryFrame frame) noexcept {
    frame.schemaVersion = kTelemetrySchemaVersion;
    frame.sequence = seq_++;  // advances even if dropped so the reader sees a gap
    if (!ch_->readerAttached_.load(std::memory_order_relaxed)) {
        discardedNoReader_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (frame.componentCount > std::uint32_t(kMaxTelemetryComponents)) frame.componentCount = kMaxTelemetryComponents;
    if (frame.xyCount > std::uint32_t(kMaxTelemetryXy)) frame.xyCount = kMaxTelemetryXy;
    if (!ch_->queue_.tryPush(frame)) {
        droppedFull_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    published_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void TelemetryReader::convert(const TelemetryFrame& f, DisplayFrame& out) noexcept {
    out.raw = f;
    for (std::uint32_t i = 0; i < f.componentCount && i < std::uint32_t(kMaxTelemetryComponents); ++i) {
        const ComponentTelemetry& c = f.components[i];
        DisplayComponent& d = out.components[i];
        d.componentId = c.componentId;
        d.temperatureC = kelvinToCelsius(double(c.temperatureK));
        d.dissipationW = double(c.dissipationW);
        d.currentA = double(c.currentA);
        d.voltageV = double(c.voltageV);
    }
}

bool TelemetryReader::poll(DisplayFrame& out, double nowSeconds) noexcept {
    TelemetryFrame f;
    if (!ch_->queue_.tryPop(f)) return false;
    convert(f, out);
    out.missedBefore = 0;
    out.generationChanged = out.epochChanged = false;
    out.schemaMismatch = f.schemaVersion != kTelemetrySchemaVersion;
    if (haveLast_) {
        out.missedBefore = f.sequence - (lastSeq_ + 1);  // unsigned wrap-safe
        out.generationChanged = f.graphGeneration != lastGeneration_;
        out.epochChanged = f.epoch != lastEpoch_;
        totalMissed_ += out.missedBefore;
    }
    haveLast_ = true;
    lastSeq_ = f.sequence;
    lastGeneration_ = f.graphGeneration;
    lastEpoch_ = f.epoch;
    lastArrival_ = nowSeconds;
    ++framesRead_;
    return true;
}

bool TelemetryReader::pollLatest(DisplayFrame& out, double nowSeconds) noexcept {
    if (!poll(out, nowSeconds)) return false;
    DisplayFrame tmp;
    std::uint32_t missed = out.missedBefore;
    bool gen = out.generationChanged, ep = out.epochChanged;
    while (poll(tmp, nowSeconds)) {
        missed += tmp.missedBefore;
        gen = gen || tmp.generationChanged;
        ep = ep || tmp.epochChanged;
        out = tmp;
    }
    out.missedBefore = missed;
    out.generationChanged = gen;
    out.epochChanged = ep;
    return true;
}

bool TelemetryReader::isStale(double nowSeconds) const noexcept {
    if (!haveLast_) return true;
    return !(nowSeconds - lastArrival_ <= kStaleAfterSeconds);  // NaN -> stale
}

void TelemetryReader::close() noexcept {
    if (ch_) ch_->readerAttached_.store(false, std::memory_order_relaxed);
}

}  // namespace cd::analysis
