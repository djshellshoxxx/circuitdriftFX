// CircuitDrift plugin UI: UX-01 direct-manipulation mappings as pure functions (no JUCE), so the
// 200 px / sqrt2-pinch / age-brush contracts are unit-testable. All pixel inputs are LOGICAL pixels
// (screen delta divided by UI scale exactly once; JUCE event coordinates under the editor's scale
// transform already are logical).
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>

namespace cd::plugin::ui::gesture {

constexpr double kBiasSpanPx = 200.0;   // vertical drag that spans the whole normalized bias range
constexpr double kAgePathPx = 400.0;    // brush path length that adds 100% condition-age fraction
constexpr double kFineScale = 0.1;      // Shift
constexpr double kPinchMin = 0.5, kPinchMax = 2.0;
constexpr double kKeyStep = 0.01, kKeyStepFine = 0.001;  // arrows / Shift+arrows (normalized)
constexpr double kMinHitPx = 32.0;      // hit regions are expanded to at least this size

inline double clamp01(double v) noexcept { return std::clamp(v, 0.0, 1.0); }

// Tube / Bias: normalized bias delta = -dyLogical/200 (up = more), clamp 0..1, Shift = fine x0.1.
inline double biasFromDrag(double startNormalized, double dyLogical, bool fine) noexcept {
    return clamp01(startNormalized - dyLogical / kBiasSpanPx * (fine ? kFineScale : 1.0));
}

inline double clampPinch(double r) noexcept { return std::clamp(r, kPinchMin, kPinchMax); }

// Capacitor bank / Supply: C_new = C_start * r^2, r clamped 0.5..2 per gesture, clamped to the
// parameter's own range (manifest bounds).
inline double supplyCapacitance(double cStart, double ratio, double cMin, double cMax) noexcept {
    const double r = clampPinch(ratio);
    return std::clamp(cStart * r * r, cMin, cMax);
}
// Sag macro contribution of the same pinch: delta = -log2(r)/4 (informational; the Sag macro itself
// is never edited by this gesture so macro/micro automation cannot fight).
inline double sagContribution(double ratio) noexcept { return -std::log2(clampPinch(ratio)) / 4.0; }
// Mouse wheel over the selected Supply target: one notch (deltaY = 1) scales r by sqrt2^0.5.
inline double wheelRatio(double wheelDeltaY) noexcept { return std::pow(std::sqrt(2.0), std::clamp(wheelDeltaY, -4.0, 4.0) * 0.5); }

// Resistor cluster / Age brush: signed accumulated path length / 400 added to the age fraction.
inline double ageFromPath(double startFraction, double pathLengthPx, bool reverse) noexcept {
    return clamp01(startFraction + (reverse ? -1.0 : 1.0) * pathLengthPx / kAgePathPx);
}

// Arrows adjust +-1% (Shift +-0.1%) of the normalized range.
inline double keyAdjust(double normalized, int direction, bool fine) noexcept {
    return clamp01(normalized + direction * (fine ? kKeyStepFine : kKeyStep));
}

// ---- Numeric entry ---------------------------------------------------------------------------------
struct ParsedQuantity {
    bool ok = false;
    double value = 0;       // in base units (F, V, ...) or the plain number when the unit is ""
    std::string error;
};
// Parses "470", "470u", "470 uF", "0.47mF", "1e-4 F" into base units. `unitSymbol` is the expected
// unit letter(s) ("F", "V", "" for a plain number); a number without prefix is multiplied by
// `defaultScale` (e.g. 1e-6 so a bare "470" means 470 uF). Rejects trailing garbage.
inline ParsedQuantity parseQuantity(const std::string& text, const std::string& unitSymbol, double defaultScale = 1.0) {
    ParsedQuantity r;
    std::string s;
    for (char c : text) if (!std::isspace(static_cast<unsigned char>(c))) s.push_back(c);
    if (s.empty()) { r.error = "Enter a number"; return r; }
    char* end = nullptr;
    const double num = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || !std::isfinite(num)) { r.error = "Not a number"; return r; }
    std::string rest(end);
    double scale = defaultScale;
    auto lower = [](std::string v) { for (auto& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return v; };
    const std::string unit = lower(unitSymbol);
    if (!rest.empty()) {
        if (!unit.empty() && lower(rest) == unit) {
            scale = 1.0;  // explicit base unit, e.g. "0.00047 F"
        } else {
            std::size_t used = 1;
            double pf = 0;
            if (rest.compare(0, 2, "\xC2\xB5") == 0) { pf = 1e-6; used = 2; }
            else switch (rest[0]) {
                case 'p': pf = 1e-12; break;
                case 'n': pf = 1e-9; break;
                case 'u': pf = 1e-6; break;
                case 'm': pf = 1e-3; break;
                case 'k': case 'K': pf = 1e3; break;
                case 'M': pf = 1e6; break;
                default: break;
            }
            const std::string tail = pf != 0 ? rest.substr(used) : rest;
            if (pf == 0 || (!tail.empty() && (unit.empty() || lower(tail) != unit))) {
                r.error = "Unrecognised unit '" + rest + "'";
                return r;
            }
            scale = pf;
        }
    }
    r.ok = true;
    r.value = num * scale;
    return r;
}

}  // namespace cd::plugin::ui::gesture
