// CircuitDrift uimodel: telemetry -> display binding (UX-10 "unit/nominal labels correct", "no stale
// frames cross graph generation"; 06-interface "Engine telemetry -> analysis").
// Owner: cd_uimodel. UI/message thread only (allocates when updating). Consumes a
// cd::analysis::TelemetryReader; never writes to the engine, so closing, stalling or destroying
// the binding cannot change engine output (the writer just discards/drops frames).
// Values carry units (degC, V, A, W) and are never extrapolated: when no frame arrived within
// kStaleAfterSeconds the last values stay visible flagged `stale`; skipped frames set `gap`;
// a graph-generation or epoch change clears the previous values first (`tracesCleared`).
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "analysis/Telemetry.h"

namespace cd::uimodel {

struct DisplayValue {
    double value = 0;
    const char* unit = "";   // "\xC2\xB0""C", "V", "A", "W"
    bool valid = false;      // false: never reported (shows "--")
    std::string text() const;  // "85.3 \xC2\xB0""C", "-1.20 V", "--"
};

struct ComponentView {
    std::uint32_t componentId = 0;
    DisplayValue temperature, dissipation, current, voltage;
};

struct TelemetryView {
    bool haveFrame = false;
    bool stale = true;             // no frame within kStaleAfterSeconds (or none ever)
    bool gap = false;              // frames were lost immediately before the shown frame
    std::uint64_t totalMissed = 0;
    bool generationChanged = false, epochChanged = false, schemaMismatch = false;
    std::int64_t tracesCleared = 0;     // generation/epoch changes seen (clear plotted traces)
    std::uint32_t graphGeneration = 0;
    std::int64_t sampleIndex = 0;       // timestamp of the frame (sample time, not arrival)
    DisplayValue railVolts, railNominalVolts;
    double railFraction = 0;            // rail / V_nominal
    bool railValid = false;
    std::uint32_t fallbackFlags = 0;
    bool solverFallback = false, bypassed = false, limiterActive = false;
    std::vector<ComponentView> components;
};

class TelemetryBinding {
public:
    explicit TelemetryBinding(analysis::TelemetryReader& reader) noexcept : reader_(&reader) {}
    ~TelemetryBinding() { detach(); }
    TelemetryBinding(const TelemetryBinding&) = delete;
    TelemetryBinding& operator=(const TelemetryBinding&) = delete;

    // Drains the channel to the newest frame (if any) and refreshes the staleness flag.
    // nowSeconds: monotonic UI clock. Returns true when a new frame was applied.
    bool update(double nowSeconds);
    const TelemetryView& view() const noexcept { return view_; }
    // GUI closed / minimized: detaches the reader so the writer discards instead of queueing.
    void detach() noexcept;
    bool attached() const noexcept { return reader_ != nullptr; }

private:
    analysis::TelemetryReader* reader_;
    TelemetryView view_;
};

// Formatting helpers (fixed, locale independent).
std::string formatTemperatureC(double c);
std::string formatVolts(double v);
std::string formatAmps(double a);
std::string formatWatts(double w);

}  // namespace cd::uimodel
