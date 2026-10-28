// CircuitDrift core: parameter registry (CD-F06..F25 extension IDs 1000-1999).
// Ownership: static, immutable, generated from docs/planned-features/parameters.json.
// Threading: read-only after static init; safe from any thread, noexcept, no allocation.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace cd::core {

enum class ParamKind : uint8_t { Float, Int, Bool, Enum };
enum class ParamMapping : uint8_t { Linear, Log };
enum class ParamScope : uint8_t { Global, SlotA, SlotB };

struct ParamSpec {
    const char* id;
    uint32_t hostId;        // permanent; never reassigned once published
    const char* featureId;
    ParamKind kind;
    double min, max, def;   // in the unit below
    const char* unit;
    ParamMapping mapping;
    double smoothingMs;     // 0 => exact event boundary; for bool/enum the effect transition
    bool automatable;
    ParamScope scope;
};

inline constexpr ParamSpec kParams[] = {
#include "ParameterRegistry.generated.inc"
};
inline constexpr std::size_t kParamCount = sizeof(kParams) / sizeof(kParams[0]);

const ParamSpec* findById(const char* id) noexcept;
const ParamSpec* findByHostId(uint32_t hostId) noexcept;

// Normalized u in [0,1] <-> plain value. Out-of-range/non-finite u is clamped (NaN -> default).
double toPlain(const ParamSpec& p, double u) noexcept;
double toNormalized(const ParamSpec& p, double plain) noexcept;

// Per-sample smoothing coefficient a = exp(-1/(Fs*tau)); tau=0 -> 0 (exact boundary).
double smoothingCoeff(double sampleRate, double tauSeconds) noexcept;

}  // namespace cd::core
