// CircuitDrift uimodel: telemetry binding. See TelemetryBinding.h. UI thread only.
#include "uimodel/TelemetryBinding.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace cd::uimodel {

namespace {
constexpr const char* kDegC = "\xC2\xB0" "C";
std::string fmt(const char* f, double v) {
    char b[48];
    std::snprintf(b, sizeof b, f, v);
    return b;
}
DisplayValue mk(double v, const char* unit) {
    DisplayValue d;
    d.value = v; d.unit = unit; d.valid = std::isfinite(v);
    return d;
}
}  // namespace

std::string formatTemperatureC(double c) { return std::isfinite(c) ? fmt("%.1f ", c) + kDegC : "--"; }
std::string formatVolts(double v) { return std::isfinite(v) ? fmt("%.2f V", v) : "--"; }
std::string formatAmps(double a) {
    if (!std::isfinite(a)) return "--";
    return std::fabs(a) < 1.0 && a != 0.0 ? fmt("%.1f mA", a * 1000.0) : fmt("%.3f A", a);
}
std::string formatWatts(double w) { return std::isfinite(w) ? fmt("%.2f W", w) : "--"; }

std::string DisplayValue::text() const {
    if (!valid) return "--";
    const std::string u = unit;
    if (u == kDegC) return formatTemperatureC(value);
    if (u == "V") return formatVolts(value);
    if (u == "A") return formatAmps(value);
    if (u == "W") return formatWatts(value);
    return fmt("%g ", value) + u;
}

void TelemetryBinding::detach() noexcept {
    if (!reader_) return;
    reader_->close();
    reader_ = nullptr;
    view_.stale = true;
}

bool TelemetryBinding::update(double nowSeconds) {
    if (!reader_) return false;
    analysis::DisplayFrame f;
    const bool got = reader_->pollLatest(f, nowSeconds);
    view_.stale = reader_->isStale(nowSeconds);
    view_.totalMissed = reader_->totalMissed();
    if (!got) return false;

    if (f.generationChanged || f.epochChanged) {
        view_.components.clear();   // no stale frame crosses a graph generation
        ++view_.tracesCleared;
    }
    view_.haveFrame = true;
    view_.gap = f.missedBefore > 0;
    view_.generationChanged = f.generationChanged;
    view_.epochChanged = f.epochChanged;
    view_.schemaMismatch = f.schemaMismatch;
    const analysis::TelemetryFrame& r = f.raw;
    view_.graphGeneration = r.graphGeneration;
    view_.sampleIndex = r.sampleIndex;
    view_.fallbackFlags = r.fallbackFlags;
    view_.solverFallback = (r.fallbackFlags & analysis::kFallbackSolverDiverged) != 0;
    view_.bypassed = (r.fallbackFlags & analysis::kFallbackBypassed) != 0;
    view_.limiterActive = (r.fallbackFlags & analysis::kFallbackLimiterActive) != 0;
    view_.railValid = !f.schemaMismatch && (r.validityFlags & analysis::kValidRail) != 0;
    view_.railVolts = view_.railValid ? mk(double(r.railVolts), "V") : DisplayValue{};
    view_.railNominalVolts = view_.railValid ? mk(double(r.railNominalVolts), "V") : DisplayValue{};
    view_.railFraction = view_.railValid ? double(r.railFraction) : 0.0;

    view_.components.clear();
    if (!f.schemaMismatch) {
        const std::uint32_t n = std::min<std::uint32_t>(r.componentCount, std::uint32_t(analysis::kMaxTelemetryComponents));
        for (std::uint32_t i = 0; i < n; ++i) {
            if ((r.components[i].flags & 1u) == 0) continue;   // record not valid
            const analysis::DisplayComponent& c = f.components[i];
            ComponentView v;
            v.componentId = c.componentId;
            v.temperature = mk(c.temperatureC, kDegC);
            v.dissipation = mk(c.dissipationW, "W");
            v.current = mk(c.currentA, "A");
            v.voltage = mk(c.voltageV, "V");
            view_.components.push_back(v);
        }
    }
    return true;
}

}  // namespace cd::uimodel
