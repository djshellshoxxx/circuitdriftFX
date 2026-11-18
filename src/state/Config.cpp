#include "state/Config.h"
#include <cmath>

namespace cd::state {
namespace {
struct Route { std::uint8_t where; std::uint8_t pos; };  // where: 0 = A, 1 = B, 2 = global
constexpr std::array<Route, core::kParamCount> makeRoutes() {
    std::array<Route, core::kParamCount> r{};
    for (std::size_t p = 0; p < kSlotParamCount; ++p) {
        r[kSlotAIndices[p]] = {0, std::uint8_t(p)};
        r[kSlotBIndices[p]] = {1, std::uint8_t(p)};
    }
    for (std::size_t p = 0; p < kGlobalParamCount; ++p) r[kGlobalIndices[p]] = {2, std::uint8_t(p)};
    return r;
}
constexpr auto kRoutes = makeRoutes();
static_assert(kSlotParamCount < 256 && kGlobalParamCount < 256);
}  // namespace

SlotConfig::SlotConfig() noexcept {
    for (std::size_t p = 0; p < kSlotParamCount; ++p) params[p] = core::kParams[kSlotAIndices[p]].def;
}
GlobalConfig::GlobalConfig() noexcept {
    for (std::size_t p = 0; p < kGlobalParamCount; ++p) params[p] = core::kParams[kGlobalIndices[p]].def;
}

double Config::get(std::size_t i) const noexcept {
    const Route r = kRoutes[i];
    return r.where == 2 ? global.params[r.pos] : slots[r.where].params[r.pos];
}
void Config::set(std::size_t i, double v) noexcept {
    const Route r = kRoutes[i];
    if (r.where == 2) global.params[r.pos] = v; else slots[r.where].params[r.pos] = v;
}

core::ParamValues Config::toParams() const noexcept {
    core::ParamValues v{};
    for (std::size_t i = 0; i < core::kParamCount; ++i) v[i] = get(i);
    return v;
}

Config Config::fromParams(const core::ParamValues& v, const Config* meta) {
    Config c;
    if (meta) for (int s = 0; s < 2; ++s) { c.slots[s].blueprintId = meta->slots[s].blueprintId; c.slots[s].seed = meta->slots[s].seed; }
    for (std::size_t i = 0; i < core::kParamCount; ++i) c.set(i, v[i]);
    return c;
}

bool validateParamValue(std::size_t i, double v, std::string& error) {
    if (i >= core::kParamCount) { error = "parameter index out of range"; return false; }
    const core::ParamSpec& s = core::kParams[i];
    if (!std::isfinite(v)) { error = std::string("non-finite value: ") + s.id; return false; }
    if (v < s.min || v > s.max) { error = std::string("value out of range (not clamped): ") + s.id; return false; }
    if (s.kind != core::ParamKind::Float && v != std::floor(v)) { error = std::string("non-integral discrete value: ") + s.id; return false; }
    return true;
}

bool validBlueprintId(const std::string& id) {
    if (id.empty() || id.size() > kBlueprintIdMaxLen) return false;
    for (unsigned char ch : id) if (ch < 0x20 || ch > 0x7e) return false;
    return true;
}

bool Config::validate(std::string& error) const {
    for (std::size_t i = 0; i < core::kParamCount; ++i) if (!validateParamValue(i, get(i), error)) return false;
    for (int s = 0; s < 2; ++s) {
        if (!validBlueprintId(slots[s].blueprintId)) { error = "invalid blueprint id"; return false; }
        if (slots[s].seed > kMaxSeed) { error = "seed exceeds 2^53"; return false; }
    }
    error.clear();
    return true;
}

std::size_t Config::approxBytes() const noexcept {
    return sizeof(Config) + slots[0].blueprintId.capacity() + slots[1].blueprintId.capacity();
}

}  // namespace cd::state
