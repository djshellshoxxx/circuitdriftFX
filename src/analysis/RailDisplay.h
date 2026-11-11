// CircuitDrift analysis: power rail waveform display math (UX-05).
// Owner: cd_analysis. Worker/UI thread. RailHistory keeps 10 s of 1 kHz min/mean/max bins in a
// preallocated ring (allocation only in the constructor); envelope()/estimateRecovery() do not
// allocate. Rail values are the selected internal DC rail, never AC mains RMS (labels differ).
// Recovery time is measured on the 1 ms mean bins with linear interpolation between bins; it is
// limited by the 1 kHz telemetry (cursor readouts must quote the 1 ms bin, not a sample).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cd::analysis {

inline constexpr int kRailBinsPerSecond = 1000;
inline constexpr std::size_t kRailHistoryBins = 10 * kRailBinsPerSecond;

struct RailBin {
    float minV = 0, meanV = 0, maxV = 0;
    std::int64_t binIndex = -1;  // absolute 1 ms bin index (sample-derived), -1 = empty
};

enum class RailKind { InternalDc, AcMainsInput };
// Distinct, non-interchangeable labels (UX-05: AC wall RMS is not the DC rail).
const char* railKindLabel(RailKind k) noexcept;
const char* railKindUnits(RailKind k) noexcept;

struct EnvelopeColumn {
    float minV = 0, maxV = 0, meanV = 0;
    bool hasData = false;
    bool hasGap = false;  // at least one 1 ms bin in the column is missing (dropped telemetry)
};

struct EnvelopeInfo {
    std::size_t columns = 0;
    std::int64_t firstBin = 0;  // absolute bin of column 0 start
    double binsPerColumn = 0;
    bool stale = false;  // newest data older than 250 ms vs nowBin
    bool empty = true;
};

class RailHistory {
public:
    RailHistory();
    void clear() noexcept;  // graph/epoch change
    // Non-finite values or bins older than the window are rejected (returns false).
    bool push(std::int64_t binIndex, float minV, float meanV, float maxV) noexcept;
    bool get(std::int64_t binIndex, RailBin& out) const noexcept;
    std::int64_t newestBin() const noexcept { return newest_; }  // -1 if empty

    // Min/max envelope by display column over the last spanMs (clamped to [100, 10000]) ending at
    // endBin (use newestBin()). out must hold `columns` entries. nowBin is the current time in
    // bins for stale marking (pass newestBin() when live).
    EnvelopeInfo envelope(std::int64_t endBin, double spanMs, EnvelopeColumn* out, std::size_t columns,
                          std::int64_t nowBin) const noexcept;

private:
    std::vector<RailBin> ring_;
    std::int64_t newest_ = -1;
};

enum class RecoveryStatus { Ok, NotRecovered, RejectedOverlap, NoEvent, InsufficientData };

struct RecoveryParams {
    std::int64_t startBin = 0;  // first bin of the analysed window
    std::int64_t endBin = 0;    // last bin (inclusive)
    int preBins = 50;           // pre-event baseline length
    double minDroopFraction = 0.02;  // droop must exceed this fraction of pre-event level
    double recoverFraction = 0.95;   // recovered when 95% of droop is regained
    double targetRecoveryMs = -1;    // sync target (separate from the measurement), <0 = none
};

struct RecoveryResult {
    RecoveryStatus status = RecoveryStatus::NoEvent;
    double preLevelV = 0;
    double minRailV = 0;
    double minTimeMs = 0;       // absolute time of the minimum bin
    double measuredRecoveryMs = 0;  // from minimum to 95% recovery; valid only for Ok
    double targetRecoveryMs = -1;   // echoed; shown as a separate line/label
};
const char* recoveryStatusLabel(RecoveryStatus s) noexcept;  // "not recovered" etc.

RecoveryResult estimateRecovery(const RailHistory& h, const RecoveryParams& p) noexcept;

}  // namespace cd::analysis
