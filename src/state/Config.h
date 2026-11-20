// CircuitDrift state: canonical configuration model (CD-F09/F24). Message/worker thread only.
//
// Config is the exact, canonical description of "what the sound is" at the parameter level:
//   * SlotConfig x2  : the 39 per-slot registry params of one slot (21 base params, lifecycle,
//                      scenario, 16 mod values; ids slot.a.* / slot.b.*) + blueprint id + seed.
//   * GlobalConfig   : every ParamScope::Global registry param (macros, utilities, quality,
//                      key/tempo/harmonics/sidechain/network, ab.crossfade, lite.*).
// Config <-> core::ParamValues is a lossless bijection for the registry params (the meta fields
// blueprintId/seed are not host parameters and travel beside them). Canonical equality is exact
// double equality (== ; +0 == -0), blueprint id and seed equality.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include "core/ParameterRegistry.h"

namespace cd::state {

namespace detail {
constexpr std::size_t countScope(core::ParamScope s) noexcept {
    std::size_t n = 0;
    for (std::size_t i = 0; i < core::kParamCount; ++i) if (core::kParams[i].scope == s) ++n;
    return n;
}
}  // namespace detail

inline constexpr std::size_t kSlotParamCount = detail::countScope(core::ParamScope::SlotA);
inline constexpr std::size_t kGlobalParamCount = detail::countScope(core::ParamScope::Global);
static_assert(detail::countScope(core::ParamScope::SlotB) == kSlotParamCount, "slots must be symmetric");
static_assert(kSlotParamCount * 2 + kGlobalParamCount == core::kParamCount, "every param has a scope");
inline constexpr std::size_t kBlueprintIdMaxLen = 128;
inline constexpr std::uint64_t kMaxSeed = 9007199254740992ULL;  // 2^53 (schema v1 identity bound)

namespace detail {
template <std::size_t N>
constexpr std::array<std::size_t, N> scopeIndices(core::ParamScope s) noexcept {
    std::array<std::size_t, N> out{};
    std::size_t k = 0;
    for (std::size_t i = 0; i < core::kParamCount; ++i) if (core::kParams[i].scope == s) out[k++] = i;
    return out;
}
constexpr bool slotsAligned(const std::array<std::size_t, kSlotParamCount>& a, const std::array<std::size_t, kSlotParamCount>& b) noexcept {
    for (std::size_t i = 0; i < kSlotParamCount; ++i) {
        const std::string_view x = core::kParams[a[i]].id, y = core::kParams[b[i]].id;
        if (x.substr(0, 7) != "slot.a." || y.substr(0, 7) != "slot.b." || x.substr(7) != y.substr(7)) return false;
        const auto& p = core::kParams[a[i]];
        const auto& q = core::kParams[b[i]];
        if (p.kind != q.kind || p.min != q.min || p.max != q.max || p.def != q.def) return false;
    }
    return true;
}
}  // namespace detail

// Registry indices of each slot's params (slot-relative position -> registry index) and of globals.
inline constexpr auto kSlotAIndices = detail::scopeIndices<kSlotParamCount>(core::ParamScope::SlotA);
inline constexpr auto kSlotBIndices = detail::scopeIndices<kSlotParamCount>(core::ParamScope::SlotB);
inline constexpr auto kGlobalIndices = detail::scopeIndices<kGlobalParamCount>(core::ParamScope::Global);
static_assert(detail::slotsAligned(kSlotAIndices, kSlotBIndices), "slot A/B params must mirror each other");

constexpr std::size_t slotParamIndex(int slot, std::size_t pos) noexcept {
    return slot == 0 ? kSlotAIndices[pos] : kSlotBIndices[pos];
}

struct SlotConfig {
    std::array<double, kSlotParamCount> params{};  // by slot-relative position; registry defaults
    std::string blueprintId = "base.default";
    std::uint64_t seed = 0;

    SlotConfig() noexcept;  // registry defaults
    friend bool operator==(const SlotConfig& a, const SlotConfig& b) noexcept {
        return a.params == b.params && a.blueprintId == b.blueprintId && a.seed == b.seed;
    }
    friend bool operator!=(const SlotConfig& a, const SlotConfig& b) noexcept { return !(a == b); }
};

struct GlobalConfig {
    std::array<double, kGlobalParamCount> params{};  // by position in kGlobalIndices; registry defaults
    GlobalConfig() noexcept;
    friend bool operator==(const GlobalConfig& a, const GlobalConfig& b) noexcept { return a.params == b.params; }
    friend bool operator!=(const GlobalConfig& a, const GlobalConfig& b) noexcept { return !(a == b); }
};

struct Config {
    SlotConfig slots[2];  // [0] = A, [1] = B
    GlobalConfig global;

    // Registry-index accessors (index < kParamCount).
    double get(std::size_t registryIndex) const noexcept;
    void set(std::size_t registryIndex, double plain) noexcept;

    core::ParamValues toParams() const noexcept;
    // Params from `v`; blueprint ids and seeds are copied from `metaSource` (defaults when null).
    static Config fromParams(const core::ParamValues& v, const Config* metaSource = nullptr);

    // Every value finite, inside its registry range, integral for non-Float kinds; blueprint ids
    // 1..kBlueprintIdMaxLen printable ASCII; seed <= 2^53. Never clamps.
    bool validate(std::string& error) const;

    std::size_t approxBytes() const noexcept;

    friend bool operator==(const Config& a, const Config& b) noexcept {
        return a.slots[0] == b.slots[0] && a.slots[1] == b.slots[1] && a.global == b.global;
    }
    friend bool operator!=(const Config& a, const Config& b) noexcept { return !(a == b); }
};

// Value check shared by Config::validate and the preset reader. Returns false with a reason.
bool validateParamValue(std::size_t registryIndex, double v, std::string& error);
bool validBlueprintId(const std::string& id);

}  // namespace cd::state
