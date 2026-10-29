#include "ParameterRegistry.h"
#include <algorithm>
#include <cstring>

namespace cd::core {

const ParamSpec* findById(const char* id) noexcept {
    if (!id) return nullptr;
    for (const auto& p : kParams) if (std::strcmp(p.id, id) == 0) return &p;
    return nullptr;
}
const ParamSpec* findByHostId(uint32_t hostId) noexcept {
    for (const auto& p : kParams) if (p.hostId == hostId) return &p;
    return nullptr;
}

double toPlain(const ParamSpec& p, double u) noexcept {
    if (!std::isfinite(u)) return p.def;
    u = std::clamp(u, 0.0, 1.0);
    if (p.kind == ParamKind::Float && p.mapping == ParamMapping::Log)
        return p.min * std::pow(p.max / p.min, u);
    double v = p.min + (p.max - p.min) * u;
    if (p.kind != ParamKind::Float) v = std::round(v);  // enum: round(u*(max-min))+min
    return v;
}

double toNormalized(const ParamSpec& p, double plain) noexcept {
    if (!std::isfinite(plain)) return toNormalized(p, p.def);
    plain = std::clamp(plain, p.min, p.max);
    if (p.max == p.min) return 0.0;
    if (p.kind == ParamKind::Float && p.mapping == ParamMapping::Log)
        return std::log(plain / p.min) / std::log(p.max / p.min);
    return (plain - p.min) / (p.max - p.min);
}

double smoothingCoeff(double sampleRate, double tauSeconds) noexcept {
    if (!(tauSeconds > 0.0) || !(sampleRate > 0.0)) return 0.0;
    return std::exp(-1.0 / (sampleRate * tauSeconds));
}

}  // namespace cd::core
