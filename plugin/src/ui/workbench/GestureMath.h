// CircuitDrift plugin UI: UX-01 direct-manipulation mappings as pure functions (no JUCE), so the
// 200 px / sqrt2-pinch / age-brush contracts are unit-testable. All pixel inputs are LOGICAL pixels
// (screen delta divided by UI scale exactly once; JUCE event coordinates under the editor's scale
// transform already are logical).
#pragma once
#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>

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
    if (!std::isfinite(defaultScale) || defaultScale <= 0) { r.error = "Invalid unit scale"; return r; }
    const char* first = text.data();
    const char* last = first + text.size();
    while (first != last && std::isspace(static_cast<unsigned char>(*first))) ++first;
    if (first == last) { r.error = "Enter a number"; return r; }
    // from_chars is locale independent and length bounded. Do not erase whitespace within a
    // number ("1 0" must not become 10), and do not silently stop at an embedded NUL.
    if (*first == '+') {
        ++first;
        if (first == last || *first == '-' || *first == '+') { r.error = "Not a number"; return r; }
    }
    double num = 0;
    const auto parsed = std::from_chars(first, last, num, std::chars_format::general);
    if (parsed.ec != std::errc{} || !std::isfinite(num)) { r.error = "Not a number"; return r; }
    std::string rest;
    for (auto p = parsed.ptr; p != last; ++p)
        if (!std::isspace(static_cast<unsigned char>(*p))) rest.push_back(*p);
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
    r.value = num * scale;
    if (!std::isfinite(r.value)) { r.error = "Number out of range"; return r; }
    r.ok = true;
    return r;
}

inline std::optional<double> parseBoundedQuantity(const std::string& text, const std::string& unit,
                                                 double lo, double hi) {
    const auto parsed = parseQuantity(text, unit);
    if (!parsed.ok || parsed.value < lo || parsed.value > hi) return std::nullopt;
    return parsed.value;
}

inline std::string_view trimQuantityText(std::string_view text) noexcept {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return text;
}

// Percentage-labelled controls accept bare numbers in displayed percent units.
inline std::optional<double> parsePercentage(const std::string& text) {
    const auto percent = parseBoundedQuantity(text, "%", 0.0, 100.0);
    return percent ? std::optional<double>(*percent / 100.0) : std::nullopt;
}

inline std::optional<double> parseHarmonicTarget(const std::string& text) {
    const auto trimmed = trimQuantityText(text);
    if (trimmed == "Balanced") return 0.0;
    const bool odd = trimmed.starts_with("Odd ");
    const bool even = trimmed.starts_with("Even ");
    if (!odd && !even) return parseBoundedQuantity(text, "", -1.0, 1.0);
    const auto amount = trimQuantityText(trimmed.substr(odd ? 4 : 5));
    if (amount.empty() || amount.back() != '%') return std::nullopt;
    const auto percent = parsePercentage(std::string(amount));
    return percent ? std::optional<double>(odd ? -*percent : *percent) : std::nullopt;
}

inline std::optional<double> parseCrossfade(const std::string& text) {
    const auto trimmed = trimQuantityText(text);
    const auto separator = trimmed.find('/');
    if (separator == std::string_view::npos) return parseBoundedQuantity(text, "", 0.0, 1.0);
    const auto side = [](std::string_view value, char label) -> std::optional<double> {
        value = trimQuantityText(value);
        if (value.empty() || value.back() != label) return std::nullopt;
        value.remove_suffix(1);
        value = trimQuantityText(value);
        if (value.empty() || value.back() != '%') return std::nullopt;
        return parsePercentage(std::string(value));
    };
    const auto a = side(trimmed.substr(0, separator), 'A');
    const auto b = side(trimmed.substr(separator + 1), 'B');
    if (!a || !b || std::abs(*a + *b - 1.0) > 1e-9) return std::nullopt;
    return b;
}

}  // namespace cd::plugin::ui::gesture
