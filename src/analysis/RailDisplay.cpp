// CircuitDrift analysis: power rail display math implementation. See RailDisplay.h.
#include "analysis/RailDisplay.h"
#include "analysis/Telemetry.h"
#include <algorithm>
#include <cmath>

namespace cd::analysis {

const char* railKindLabel(RailKind k) noexcept {
    return k == RailKind::InternalDc ? "Internal DC rail" : "AC mains input (RMS)";
}
const char* railKindUnits(RailKind k) noexcept { return k == RailKind::InternalDc ? "V DC" : "V AC rms"; }

const char* recoveryStatusLabel(RecoveryStatus s) noexcept {
    switch (s) {
        case RecoveryStatus::Ok: return "recovered";
        case RecoveryStatus::NotRecovered: return "not recovered";
        case RecoveryStatus::RejectedOverlap: return "overlapping events: recovery not measured";
        case RecoveryStatus::NoEvent: return "no droop event";
        case RecoveryStatus::InsufficientData: return "insufficient data (gap)";
    }
    return "";
}

RailHistory::RailHistory() : ring_(kRailHistoryBins) {}

void RailHistory::clear() noexcept {
    for (auto& b : ring_) b = RailBin{};
    newest_ = -1;
}

bool RailHistory::push(std::int64_t bin, float mn, float mean, float mx) noexcept {
    if (bin < 0 || !std::isfinite(mn) || !std::isfinite(mean) || !std::isfinite(mx) || mn > mean || mean > mx)
        return false;
    if (newest_ >= 0 && bin <= newest_ - std::int64_t(kRailHistoryBins)) return false;
    RailBin& s = ring_[std::size_t(bin % std::int64_t(kRailHistoryBins))];
    s = RailBin{mn, mean, mx, bin};
    if (bin > newest_) newest_ = bin;
    return true;
}

bool RailHistory::get(std::int64_t bin, RailBin& out) const noexcept {
    if (bin < 0) return false;
    const RailBin& s = ring_[std::size_t(bin % std::int64_t(kRailHistoryBins))];
    if (s.binIndex != bin) return false;
    out = s;
    return true;
}

EnvelopeInfo RailHistory::envelope(std::int64_t endBin, double spanMs, EnvelopeColumn* out, std::size_t columns,
                                   std::int64_t nowBin) const noexcept {
    EnvelopeInfo info;
    info.columns = columns;
    if (!out || columns == 0 || newest_ < 0) return info;
    if (!std::isfinite(spanMs)) spanMs = 2000;
    spanMs = std::clamp(spanMs, 100.0, 10000.0);
    const std::int64_t span = std::int64_t(spanMs);
    info.firstBin = endBin - span + 1;
    info.binsPerColumn = double(span) / double(columns);
    info.stale = double(nowBin - newest_) > kStaleAfterSeconds * kRailBinsPerSecond;
    info.empty = false;
    for (std::size_t c = 0; c < columns; ++c) {
        EnvelopeColumn col;
        const std::int64_t lo = info.firstBin + std::int64_t(double(c) * info.binsPerColumn);
        std::int64_t hi = info.firstBin + std::int64_t(double(c + 1) * info.binsPerColumn);
        if (hi <= lo) hi = lo + 1;
        double sum = 0;
        int cnt = 0;
        for (std::int64_t b = lo; b < hi; ++b) {
            RailBin rb;
            if (!get(b, rb)) { col.hasGap = true; continue; }
            if (cnt == 0) { col.minV = rb.minV; col.maxV = rb.maxV; }
            col.minV = std::min(col.minV, rb.minV);
            col.maxV = std::max(col.maxV, rb.maxV);
            sum += rb.meanV;
            ++cnt;
        }
        col.hasData = cnt > 0;
        col.meanV = cnt ? float(sum / cnt) : 0.0f;
        out[c] = col;
    }
    return info;
}

RecoveryResult estimateRecovery(const RailHistory& h, const RecoveryParams& p) noexcept {
    RecoveryResult r;
    r.targetRecoveryMs = p.targetRecoveryMs;
    if (p.preBins < 1 || p.endBin <= p.startBin + p.preBins || !(p.recoverFraction > 0.0 && p.recoverFraction < 1.0) ||
        !(p.minDroopFraction > 0.0)) {
        r.status = RecoveryStatus::InsufficientData;
        return r;
    }
    RailBin b;
    double pre = 0;
    for (int i = 0; i < p.preBins; ++i) {
        if (!h.get(p.startBin + i, b)) { r.status = RecoveryStatus::InsufficientData; return r; }
        pre += b.meanV;
    }
    pre /= p.preBins;
    r.preLevelV = pre;
    const double minDroop = p.minDroopFraction * std::fabs(pre);

    std::int64_t i = p.startBin + p.preBins;
    for (; i <= p.endBin; ++i) {
        if (!h.get(i, b)) { r.status = RecoveryStatus::InsufficientData; return r; }
        if (b.meanV < pre - minDroop) break;
    }
    if (i > p.endBin) { r.status = RecoveryStatus::NoEvent; return r; }

    double m = b.meanV, railMin = b.minV, mx = m;
    std::int64_t im = i;
    double prevV = b.meanV;
    for (++i; i <= p.endBin; ++i) {
        if (!h.get(i, b)) { r.status = RecoveryStatus::InsufficientData; return r; }
        const double v = b.meanV;
        railMin = std::min<double>(railMin, b.minV);
        if (v < m) { m = v; im = i; mx = v; }
        const double D = pre - m;
        if (i > im) {
            mx = std::max(mx, v);
            // A rise then a renewed fall of >=20% of the droop is a second event: reject.
            if (mx - m >= 0.2 * D && v <= mx - 0.2 * D) {
                r.status = RecoveryStatus::RejectedOverlap;
                r.minRailV = railMin;
                r.minTimeMs = double(im);
                return r;
            }
            const double level = pre - (1.0 - p.recoverFraction) * D;
            if (v >= level) {
                const double span = v - prevV;
                const double frac = span > 0 ? (level - prevV) / span : 1.0;
                const double tCross = double(i - 1) + std::clamp(frac, 0.0, 1.0);
                r.status = RecoveryStatus::Ok;
                r.minRailV = railMin;
                r.minTimeMs = double(im);
                r.measuredRecoveryMs = tCross - double(im);
                return r;
            }
        }
        prevV = v;
    }
    r.status = RecoveryStatus::NotRecovered;
    r.minRailV = railMin;
    r.minTimeMs = double(im);
    return r;
}

}  // namespace cd::analysis
