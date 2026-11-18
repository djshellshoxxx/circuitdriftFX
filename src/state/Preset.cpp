#include "state/Preset.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace cd::state {
namespace {
using core::JsonValue;

std::string slotAId(std::size_t pos) { return core::kParams[kSlotAIndices[pos]].id; }

JsonValue blueprintJson(const SlotConfig& s, bool withSeed) {
    JsonValue b = JsonValue::makeObject();
    b.set("schemaVersion", JsonValue::makeNumber(1));
    b.set("id", JsonValue::makeString(s.blueprintId));
    b.set("components", JsonValue::makeArray());
    if (withSeed) b.set("seed", JsonValue::makeNumber(double(s.seed)));
    return b;
}

// Blueprint object must be exactly {schemaVersion,id,components:[]} (+ seed for sessions).
bool parseBlueprint(const JsonValue& b, bool withSeed, SlotConfig& s, std::string& error) {
    const std::size_t expected = withSeed ? 4 : 3;
    const JsonValue* comps = b.get("components");
    if (!b.isObject() || b.object.size() != expected || !comps || !comps->isArray() || !comps->array.empty()) {
        error = "blueprint content is not supported by the preset reader (opaque graphs need P10 validation)";
        return false;
    }
    const JsonValue* id = b.get("id");
    if (!id || !id->isString() || !validBlueprintId(id->string)) { error = "invalid blueprint id"; return false; }
    s.blueprintId = id->string;
    if (withSeed) {
        const JsonValue* sd = b.get("seed");
        if (!sd || !sd->isNumber() || !sd->numberIsExactInteger || sd->number < 0 || sd->number > double(kMaxSeed)) {
            error = "invalid blueprint seed"; return false;
        }
        s.seed = std::uint64_t(sd->number);
    }
    return true;
}

bool emptyContainer(const JsonValue& v) {
    return (v.isObject() && v.object.empty()) || (v.isArray() && v.array.empty());
}

std::vector<std::string> featuresFor(const std::vector<std::size_t>& changed) {
    std::set<std::string> f{"CD-F20"};
    for (std::size_t i : changed) f.insert(core::kParams[i].featureId);
    return {f.begin(), f.end()};
}

StateDocument baseDoc(DocKind kind, const PresetMeta& m) {
    StateDocument d;
    d.kind = kind;
    d.engineModelVersion = std::string(kEngineModelVersion);
    d.minReaderVersion = 1;
    d.author = m.author;
    d.title = m.title;
    d.tags = m.tags;
    d.createdAt = m.createdAt;
    d.macros = JsonValue::makeObject();
    d.timeline = JsonValue::makeObject();
    d.faults = JsonValue::makeArray();
    return d;
}

bool checkMetaFields(const PresetMeta& m, std::string& error) {
    if (m.title.size() > 256 || m.author.size() > 256) { error = "title/author too long"; return false; }
    return true;
}
}  // namespace

ScenePreset::ScenePreset() noexcept {
    for (std::size_t i = 0; i < kSceneGlobalCount; ++i) globals[i] = core::kParams[kSceneGlobalIndices[i]].def;
}
bool operator==(const ScenePreset& a, const ScenePreset& b) noexcept {
    return a.meta.title == b.meta.title && a.meta.author == b.meta.author && a.meta.tags == b.meta.tags &&
           a.meta.createdAt == b.meta.createdAt && a.slot == b.slot && a.globals == b.globals;
}

ScenePreset captureScene(const Config& c, int slot, PresetMeta meta) {
    ScenePreset p;
    p.meta = std::move(meta);
    p.slot = c.slots[slot];
    for (std::size_t i = 0; i < kSceneGlobalCount; ++i) p.globals[i] = c.get(kSceneGlobalIndices[i]);
    return p;
}

Config applyScene(const Config& cur, int slot, const ScenePreset& p, bool circuitOnly) {
    Config n = cur;
    n.slots[slot] = p.slot;
    if (!circuitOnly) for (std::size_t i = 0; i < kSceneGlobalCount; ++i) n.set(kSceneGlobalIndices[i], p.globals[i]);
    return n;
}

bool matchesScene(const Config& c, int slot, const ScenePreset& p, bool circuitOnly) {
    if (!(c.slots[slot] == p.slot)) return false;
    if (!circuitOnly)
        for (std::size_t i = 0; i < kSceneGlobalCount; ++i) if (!(c.get(kSceneGlobalIndices[i]) == p.globals[i])) return false;
    return true;
}

bool sceneToJson(const ScenePreset& p, std::string& out, std::string& error) {
    if (!checkMetaFields(p.meta, error)) return false;
    if (!validBlueprintId(p.slot.blueprintId)) { error = "invalid blueprint id"; return false; }
    if (p.slot.seed > kMaxSeed) { error = "seed exceeds 2^53"; return false; }
    StateDocument d = baseDoc(DocKind::Scene, p.meta);
    std::vector<std::size_t> changed;
    for (std::size_t i = 0; i < kSlotParamCount; ++i) {
        const std::size_t ri = kSlotAIndices[i];
        if (!validateParamValue(ri, p.slot.params[i], error)) return false;
        d.baseParameters.emplace_back(slotAId(i), p.slot.params[i]);
        if (p.slot.params[i] != core::kParams[ri].def) changed.push_back(ri);
    }
    for (std::size_t i = 0; i < kSceneGlobalCount; ++i) {
        const std::size_t ri = kSceneGlobalIndices[i];
        if (!validateParamValue(ri, p.globals[i], error)) return false;
        d.baseParameters.emplace_back(core::kParams[ri].id, p.globals[i]);
        if (p.globals[i] != core::kParams[ri].def) changed.push_back(ri);
    }
    d.requiredFeatures = featuresFor(changed);
    d.blueprints.push_back(blueprintJson(p.slot, false));
    d.seed = p.slot.seed;
    std::string text;
    try { text = saveState(d); } catch (const std::exception& e) { error = e.what(); return false; }
    ScenePreset check;  // never emit a document our own strict reader would refuse
    if (!sceneFromJson(text, check, error)) { error = "internal: written scene is unreadable: " + error; return false; }
    out = std::move(text);
    error.clear();
    return true;
}

bool sceneFromDocument(const StateDocument& d, ScenePreset& out, std::string& error) {
    if (d.kind != DocKind::Scene) { error = "not a scene"; return false; }
    if (d.blueprints.size() != 1) { error = "scene needs exactly one blueprint"; return false; }
    if (!d.macros.isObject() || !d.macros.object.empty() || !emptyContainer(d.timeline) || !emptyContainer(d.faults)) {
        error = "macros/timeline/faults payloads are not supported by the preset reader yet"; return false;
    }
    if (d.ui || d.checkpoint || !d.metadata.empty()) { error = "unknown or local-only fields (ui/checkpoint/metadata) are not allowed in presets"; return false; }
    ScenePreset p;
    p.meta.title = d.title; p.meta.author = d.author; p.meta.tags = d.tags; p.meta.createdAt = d.createdAt;
    if (!parseBlueprint(d.blueprints[0], false, p.slot, error)) return false;
    p.slot.seed = d.seed;
    for (const auto& kv : d.baseParameters) {
        const core::ParamSpec* spec = core::findById(kv.first.c_str());
        if (!spec) { error = "unknown parameter " + kv.first; return false; }
        const std::size_t ri = std::size_t(spec - core::kParams);
        bool placed = false;
        for (std::size_t i = 0; i < kSlotParamCount && !placed; ++i)
            if (kSlotAIndices[i] == ri) { p.slot.params[i] = kv.second; placed = true; }
        for (std::size_t i = 0; i < kSceneGlobalCount && !placed; ++i)
            if (kSceneGlobalIndices[i] == ri) { p.globals[i] = kv.second; placed = true; }
        if (!placed) { error = "parameter not allowed in a scene: " + kv.first; return false; }
        if (!validateParamValue(ri, kv.second, error)) return false;
    }
    out = std::move(p);
    error.clear();
    return true;
}

bool sceneFromJson(std::string_view text, ScenePreset& out, std::string& error) {
    StateDocument d;
    if (!loadState(text, d, error)) return false;
    return sceneFromDocument(d, out, error);
}

bool configToSessionJson(const Config& c, const PresetMeta& meta, std::string& out, std::string& error) {
    if (!checkMetaFields(meta, error) || !c.validate(error)) return false;
    StateDocument d = baseDoc(DocKind::Session, meta);
    std::vector<std::size_t> changed;
    for (std::size_t i = 0; i < core::kParamCount; ++i) {
        d.baseParameters.emplace_back(core::kParams[i].id, c.get(i));
        if (c.get(i) != core::kParams[i].def) changed.push_back(i);
    }
    d.requiredFeatures = featuresFor(changed);
    d.blueprints.push_back(blueprintJson(c.slots[0], true));
    d.blueprints.push_back(blueprintJson(c.slots[1], true));
    d.seed = c.slots[0].seed;
    std::string text;
    try { text = saveState(d); } catch (const std::exception& e) { error = e.what(); return false; }
    Config check;
    if (!configFromSessionJson(text, check, error)) { error = "internal: written session is unreadable: " + error; return false; }
    out = std::move(text);
    error.clear();
    return true;
}

bool configFromSessionJson(std::string_view text, Config& out, std::string& error) {
    StateDocument d;
    if (!loadState(text, d, error)) return false;
    if (d.kind != DocKind::Session || d.blueprints.size() != 2) { error = "not a two-slot session"; return false; }
    if (!d.macros.isObject() || !d.macros.object.empty() || !emptyContainer(d.timeline) || !emptyContainer(d.faults)) {
        error = "macros/timeline/faults payloads are not supported by the preset reader yet"; return false;
    }
    if (d.ui || d.checkpoint || !d.metadata.empty()) { error = "unknown or local-only fields are not allowed"; return false; }
    Config c;
    for (int s = 0; s < 2; ++s) if (!parseBlueprint(d.blueprints[std::size_t(s)], true, c.slots[s], error)) return false;
    if (c.slots[0].seed != d.seed) { error = "document seed does not match slot A seed"; return false; }
    if (d.baseParameters.size() != core::kParamCount) { error = "session must list every parameter"; return false; }
    std::vector<bool> seen(core::kParamCount, false);
    for (const auto& kv : d.baseParameters) {
        const core::ParamSpec* spec = core::findById(kv.first.c_str());
        if (!spec) { error = "unknown parameter " + kv.first; return false; }
        const std::size_t ri = std::size_t(spec - core::kParams);
        if (seen[ri]) { error = "duplicate parameter " + kv.first; return false; }
        seen[ri] = true;
        if (!validateParamValue(ri, kv.second, error)) return false;
        c.set(ri, kv.second);
    }
    out = std::move(c);
    error.clear();
    return true;
}

}  // namespace cd::state
