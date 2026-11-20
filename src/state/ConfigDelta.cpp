#include "state/ConfigDelta.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace cd::state {

std::size_t ConfigDelta::bytes() const noexcept {
    std::size_t b = sizeof(ConfigDelta) + params.capacity() * sizeof(ParamEdit) + meta.capacity() * sizeof(SlotMetaEdit);
    for (const auto& m : meta) b += m.beforeBlueprint.capacity() + m.afterBlueprint.capacity();
    return b;
}

ConfigDelta diff(const Config& a, const Config& b) {
    ConfigDelta d;
    for (std::size_t i = 0; i < core::kParamCount; ++i) {
        const double x = a.get(i), y = b.get(i);
        if (!(x == y)) d.params.push_back({std::uint32_t(i), x, y});
    }
    for (int s = 0; s < 2; ++s) {
        const SlotConfig& p = a.slots[s];
        const SlotConfig& q = b.slots[s];
        if (p.blueprintId != q.blueprintId || p.seed != q.seed)
            d.meta.push_back({std::uint8_t(s), p.blueprintId, q.blueprintId, p.seed, q.seed});
    }
    return d;
}

bool canReplayDelta(const Config& current, const ConfigDelta& delta, bool forward) noexcept {
    std::array<bool, core::kParamCount> seen{};
    for (const auto& edit : delta.params) {
        if (edit.index >= core::kParamCount || seen[edit.index]) return false;
        seen[edit.index] = true;
        const auto& spec = core::kParams[edit.index];
        for (double value : {edit.before, edit.after}) {
            if (!std::isfinite(value) || value < spec.min || value > spec.max ||
                (spec.kind != core::ParamKind::Float && value != std::floor(value))) return false;
        }
        if (current.get(edit.index) != (forward ? edit.before : edit.after)) return false;
    }
    bool seenSlot[2] = {};
    for (const auto& meta : delta.meta) {
        if (meta.slot >= 2 || seenSlot[meta.slot] ||
            !validBlueprintId(meta.beforeBlueprint) || !validBlueprintId(meta.afterBlueprint) ||
            meta.beforeSeed > kMaxSeed || meta.afterSeed > kMaxSeed) return false;
        seenSlot[meta.slot] = true;
        const auto& slot = current.slots[meta.slot];
        if (meta.beforeBlueprint != meta.afterBlueprint &&
            slot.blueprintId != (forward ? meta.beforeBlueprint : meta.afterBlueprint)) return false;
        if (meta.beforeSeed != meta.afterSeed &&
            slot.seed != (forward ? meta.beforeSeed : meta.afterSeed)) return false;
    }
    return true;
}

void applyForward(Config& c, const ConfigDelta& d) {
    for (const auto& e : d.params) c.set(e.index, e.after);
    for (const auto& m : d.meta) {
        if (m.beforeBlueprint != m.afterBlueprint) c.slots[m.slot].blueprintId = m.afterBlueprint;
        if (m.beforeSeed != m.afterSeed) c.slots[m.slot].seed = m.afterSeed;
    }
}
void applyReverse(Config& c, const ConfigDelta& d) {
    for (const auto& e : d.params) c.set(e.index, e.before);
    for (const auto& m : d.meta) {
        if (m.beforeBlueprint != m.afterBlueprint) c.slots[m.slot].blueprintId = m.beforeBlueprint;
        if (m.beforeSeed != m.afterSeed) c.slots[m.slot].seed = m.beforeSeed;
    }
}
void applyForward(core::ParamValues& v, const ConfigDelta& d) noexcept {
    for (const auto& e : d.params) v[e.index] = e.after;
}

void mergeInto(ConfigDelta& last, const ConfigDelta& next) {
    for (const auto& e : next.params) {
        auto it = std::find_if(last.params.begin(), last.params.end(), [&](const ParamEdit& x) { return x.index == e.index; });
        if (it == last.params.end()) last.params.push_back(e); else it->after = e.after;
    }
    last.params.erase(std::remove_if(last.params.begin(), last.params.end(), [](const ParamEdit& e) { return e.before == e.after; }),
                      last.params.end());
    std::sort(last.params.begin(), last.params.end(), [](const ParamEdit& a, const ParamEdit& b) { return a.index < b.index; });
    for (const auto& m : next.meta) {
        auto it = std::find_if(last.meta.begin(), last.meta.end(), [&](const SlotMetaEdit& x) { return x.slot == m.slot; });
        if (it == last.meta.end()) last.meta.push_back(m);
        else { it->afterBlueprint = m.afterBlueprint; it->afterSeed = m.afterSeed; }
    }
    last.meta.erase(std::remove_if(last.meta.begin(), last.meta.end(), [](const SlotMetaEdit& m) {
                        return m.beforeBlueprint == m.afterBlueprint && m.beforeSeed == m.afterSeed;
                    }), last.meta.end());
    std::sort(last.meta.begin(), last.meta.end(), [](const SlotMetaEdit& a, const SlotMetaEdit& b) { return a.slot < b.slot; });
}

Config applyAB(const Config& cur, ABOp op, bool mirror) {
    Config n = cur;
    constexpr std::size_t kXfade = core::indexOf("ab.crossfade");
    static_assert(kXfade < core::kParamCount, "registry must define ab.crossfade");
    switch (op) {
        case ABOp::CopyAtoB: n.slots[1] = cur.slots[0]; break;
        case ABOp::CopyBtoA: n.slots[0] = cur.slots[1]; break;
        case ABOp::Swap:
            std::swap(n.slots[0], n.slots[1]);
            if (mirror) n.set(kXfade, 1.0 - cur.get(kXfade));
            break;
    }
    return n;
}

ConfigDelta makeABDelta(const Config& cur, ABOp op, bool mirror) { return diff(cur, applyAB(cur, op, mirror)); }

}  // namespace cd::state
