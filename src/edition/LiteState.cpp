// CircuitDrift edition: LITE state, upgrade export, full import, compatibility (see LiteState.h).
// Owner: cd_edition. Non-audio only.
#include "edition/LiteState.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include "blueprint/BlueprintGraph.h"
#include "core/ParameterRegistry.h"

namespace cd::edition {
namespace {

using core::JsonValue;
constexpr double kMaxSafeInt = 9007199254740992.0;  // 2^53

bool exactInt(const JsonValue* v, double lo, double hi) {
    return v && v->isNumber() && v->numberIsExactInteger && v->number >= lo && v->number <= hi;
}
bool unitNumber(const JsonValue* v, double& out) {
    if (!v || !v->isNumber() || !std::isfinite(v->number) || v->number < 0.0 || v->number > 1.0) return false;
    out = v->number;
    return true;
}

// Fixed LITE base controls (registry id -> plain value), LiteMapping v1.
const std::vector<std::pair<const char*, double>>& liteFixedParams() {
    static const std::vector<std::pair<const char*, double>> k = {
        {"input.gain", 0.0}, {"output.gain", 0.0}, {"mix", 1.0}, {"gainMatch.enabled", 1.0}, {"output.softClip", 1.0},
        {"hpf.enabled", 1.0}, {"hpf.frequency", kLiteHpfHz}, {"analogNoise.enabled", 0.0}, {"pitchDrift.enabled", 0.0},
        {"quality.mode", 0.0 /* Eco */}, {"quality.oversampling", 0.0 /* Auto = Eco 4x */}, {"quality.phase", 0.0 /* FIR */},
        {"quality.zeroLatency", 0.0}};
    return k;
}

const char* kMacroIds[4] = {"macro.age", "macro.sag", "macro.drift", "macro.warmth"};
const char* kMacroKeys[4] = {"age", "sag", "drift", "warmth"};

bool emptyPayload(const JsonValue& v) {
    return v.isNull() || (v.isObject() && v.object.empty()) || (v.isArray() && v.array.empty());
}

// Resolves macro values from baseParameters macro.* and the macros object; they must agree.
bool sceneMacros(const state::StateDocument& d, const std::map<std::string, double>& params, double m[4], std::string& err) {
    for (int k = 0; k < 4; ++k) {
        const auto it = params.find(kMacroIds[k]);
        m[k] = it != params.end() ? it->second : core::findById(kMacroIds[k])->def;
    }
    if (d.macros.isNull()) return true;
    if (!d.macros.isObject()) { err = "macros must be an object"; return false; }
    for (const auto& kv : d.macros.object) {
        int k = 0;
        while (k < 4 && kv.first != kMacroKeys[k]) ++k;
        double v = 0;
        if (k == 4 || !unitNumber(&kv.second, v)) { err = "unsupported macros entry " + kv.first; return false; }
        if (params.count(kMacroIds[k]) && params.at(kMacroIds[k]) != v) { err = std::string("macros.") + kMacroKeys[k] + " disagrees with " + kMacroIds[k]; return false; }
        m[k] = v;
    }
    return true;
}

std::string canonicalBlueprint(const JsonValue& v, bool& ok) {
    blueprint::Blueprint bp;
    blueprint::Diagnostics diag;
    ok = blueprint::parseBlueprint(v, bp, diag);
    return ok ? blueprint::writeBlueprint(bp) : std::string();
}

}  // namespace

bool saveLiteState(const LiteState& s, std::string& out, std::string& err) {
    if (!(std::isfinite(s.knobs.sag) && s.knobs.sag >= 0 && s.knobs.sag <= 1 && std::isfinite(s.knobs.thermalAge) &&
          s.knobs.thermalAge >= 0 && s.knobs.thermalAge <= 1)) { err = "knob out of range"; return false; }
    if (double(s.seed) > kMaxSafeInt) { err = "seed exceeds 2^53"; return false; }
    if (s.qualityRevision != kLiteQualityRevision) { err = "unsupported quality revision"; return false; }
    if (!s.ui.isObject()) { err = "ui must be an object"; return false; }
    JsonValue o = JsonValue::makeObject();
    o.set("schemaVersion", JsonValue::makeNumber(kLiteSchemaVersion));
    o.set("edition", JsonValue::makeString("lite"));
    o.set("liteMappingVersion", JsonValue::makeNumber(kLiteMappingVersion));
    o.set("sag", JsonValue::makeNumber(s.knobs.sag));
    o.set("thermalAge", JsonValue::makeNumber(s.knobs.thermalAge));
    o.set("seed", JsonValue::makeNumber(double(s.seed)));
    o.set("qualityRevision", JsonValue::makeNumber(s.qualityRevision));
    o.set("ui", s.ui);
    std::string text;
    try { text = core::writeJson(o); } catch (const std::exception& e) { err = e.what(); return false; }
    LiteState check;
    if (!loadLiteState(text, check, err)) return false;   // never emit text the loader rejects
    out = std::move(text);
    return true;
}

bool loadLiteState(std::string_view text, LiteState& out, std::string& err) {
    JsonValue v;
    if (!core::parseJson(text, v, err, core::JsonLimits{65536, 16})) return false;
    if (!v.isObject()) { err = "LITE state must be an object"; return false; }
    static const std::set<std::string> keys = {"schemaVersion", "edition", "liteMappingVersion", "sag", "thermalAge", "seed",
                                               "qualityRevision", "ui"};
    for (const auto& kv : v.object) if (!keys.count(kv.first)) { err = "unknown LITE state key " + kv.first; return false; }
    if (!exactInt(v.get("schemaVersion"), kLiteSchemaVersion, kLiteSchemaVersion)) { err = "unsupported LITE schemaVersion"; return false; }
    const JsonValue* ed = v.get("edition");
    if (!ed || !ed->isString() || ed->string != "lite") { err = "edition must be \"lite\""; return false; }
    if (!exactInt(v.get("liteMappingVersion"), kLiteMappingVersion, kLiteMappingVersion)) { err = "unsupported liteMappingVersion"; return false; }
    LiteState s;
    if (!unitNumber(v.get("sag"), s.knobs.sag) || !unitNumber(v.get("thermalAge"), s.knobs.thermalAge)) { err = "sag/thermalAge must be in [0,1]"; return false; }
    if (!exactInt(v.get("seed"), 0, kMaxSafeInt)) { err = "seed must be an integer in [0, 2^53]"; return false; }
    s.seed = std::uint64_t(v.get("seed")->number);
    if (!exactInt(v.get("qualityRevision"), kLiteQualityRevision, kLiteQualityRevision)) { err = "unsupported quality revision"; return false; }
    s.qualityRevision = kLiteQualityRevision;
    if (const JsonValue* ui = v.get("ui")) {
        if (!ui->isObject()) { err = "ui must be an object"; return false; }
        s.ui = *ui;
    }
    out = std::move(s);
    err.clear();
    return true;
}

bool exportLiteUpgrade(const LiteState& s, const std::string& createdAt, state::StateDocument& out, std::string& err) {
    const LiteMapped m = mapLite(s.knobs);
    JsonValue bp;
    if (!core::parseJson(liteFactoryBlueprintJson(), bp, err)) return false;
    state::StateDocument d;
    d.kind = state::DocKind::Scene;
    d.engineModelVersion = std::string(state::kEngineModelVersion);
    d.minReaderVersion = 1;
    d.title = "CircuitDrift LITE upgrade";
    d.createdAt = createdAt;
    d.tags = {"lite-upgrade"};
    d.blueprints = {bp};
    for (const auto& [id, value] : liteFixedParams()) d.baseParameters.emplace_back(id, value);
    const double mv[4] = {m.macros.age, m.macros.sag, m.macros.drift, m.macros.warmth};
    d.macros = JsonValue::makeObject();
    for (int k = 0; k < 4; ++k) {
        d.baseParameters.emplace_back(kMacroIds[k], mv[k]);
        d.macros.set(kMacroKeys[k], JsonValue::makeNumber(mv[k]));
    }
    d.timeline = JsonValue::makeObject();
    d.faults = JsonValue::makeObject();
    d.seed = s.seed;
    JsonValue prov = JsonValue::makeObject();
    prov.set("liteMappingVersion", JsonValue::makeNumber(kLiteMappingVersion));
    prov.set("sag", JsonValue::makeNumber(s.knobs.sag));
    prov.set("thermalAge", JsonValue::makeNumber(s.knobs.thermalAge));
    prov.set("qualityRevision", JsonValue::makeNumber(s.qualityRevision));
    d.metadata.emplace_back("liteUpgrade", std::move(prov));
    std::string text;
    if (!state::saveState(d, text, err)) return false;   // validated envelope
    out = std::move(d);
    return true;
}

bool sceneToEngineConfig(const state::StateDocument& d, double fs, int maxBlock, int channels, SceneEngineConfig& out, std::string& err) {
    if (d.kind != state::DocKind::Scene) { err = "full import here takes a scene (sessions/A-B use SlotEngine per slot)"; return false; }
    if (d.engineModelVersion != state::kEngineModelVersion) { err = "unsupported engineModelVersion"; return false; }
    if (d.blueprints.size() != 1) { err = "scene requires exactly one blueprint"; return false; }
    if (!emptyPayload(d.timeline) || !emptyPayload(d.faults)) { err = "timeline/fault payloads are not rendered by this engine yet"; return false; }
    std::map<std::string, double> params;
    for (const auto& [id, v] : d.baseParameters) {
        const core::ParamSpec* spec = core::findById(id.c_str());
        if (!spec || !std::isfinite(v) || v < spec->min || v > spec->max) { err = "invalid parameter " + id; return false; }
        if (!params.emplace(id, v).second) { err = "duplicate parameter " + id; return false; }
    }
    auto get = [&](const char* id) { const auto it = params.find(id); return it != params.end() ? it->second : core::findById(id)->def; };
    static const std::set<std::string> wired = {"input.gain", "output.gain", "mix", "gainMatch.enabled", "output.softClip", "hpf.enabled",
                                                "hpf.frequency", "macro.age", "macro.sag", "macro.drift", "macro.warmth",
                                                "quality.mode", "quality.oversampling", "quality.phase", "quality.zeroLatency",
                                                "slot.a.lifecycle", "slot.a.scenario"};
    for (const auto& [id, v] : params) {
        if (wired.count(id)) continue;
        if (v != core::findById(id.c_str())->def) { err = "parameter " + id + " requires a feature this engine does not render"; return false; }
    }
    // (analogNoise.enabled / pitchDrift.enabled are not wired: non-default values were rejected above.)
    runtime::QualitySettings q;
    const int mode = int(get("quality.mode")), os = int(get("quality.oversampling")), phase = int(get("quality.phase"));
    q.mode = mode == 0 ? runtime::QualityMode::Eco : mode == 2 ? runtime::QualityMode::Render : runtime::QualityMode::Studio;
    q.zeroLatency = get("quality.zeroLatency") >= 0.5;
    if (os != 0 || phase != 0) {
        static const int kFactor[6] = {0, 1, 2, 4, 8, 16};
        q.manualFactor = os == 0 ? (mode == 2 ? 16 : 4) : kFactor[os];
        q.family = phase == 1 ? dsp::OsFamily::Iir : dsp::OsFamily::Fir;
        q.mode = runtime::QualityMode::Manual;
    }
    SceneEngineConfig c;
    const std::string bpText = core::writeJson(d.blueprints[0]);
    if (!circuitFromBlueprint(bpText, fs, maxBlock, channels, q, c.prepared, err)) return false;
    double m[4];
    if (!sceneMacros(d, params, m, err)) return false;
    runtime::EngineConfig& e = c.prepared.engine;
    e.initialMacros = {m[0], m[1], m[2], m[3]};
    e.initial.inputGainDb = get("input.gain"); e.initial.outputTrimDb = get("output.gain"); e.initial.mix = get("mix");
    e.initial.autoGain = get("gainMatch.enabled") >= 0.5; e.initial.softClipEnabled = get("output.softClip") >= 0.5;
    e.initial.hpfEnabled = get("hpf.enabled") >= 0.5; e.initial.hpfCutoffHz = get("hpf.frequency");
    e.initialLifecycle = get("slot.a.lifecycle");
    e.initialScenario = blueprint::Scenario(int(get("slot.a.scenario")));
    e.macroBindings = runtime::syntheticMacroBindings();
    c.seed = d.seed;
    out = std::move(c);
    err.clear();
    return true;
}

std::string LiteCompatibility::summary() const {
    if (compatible) return "compatible with CircuitDrift LITE (liteMappingVersion 1)";
    std::string s = "not loadable in CircuitDrift LITE (" + std::to_string(issues.size()) + " issue(s)): ";
    for (std::size_t i = 0; i < issues.size(); ++i) s += (i ? "; " : "") + issues[i];
    return s;
}

LiteCompatibility checkLiteCompatible(const state::StateDocument& d, LiteState& out) {
    LiteCompatibility r;
    auto issue = [&](std::string s) { r.issues.push_back(std::move(s)); };
    if (d.kind != state::DocKind::Scene) issue("sessions and A/B dual engines are full-edition features");
    if (d.blueprints.size() != 1) {
        issue("A/B or missing blueprint");
    } else {
        JsonValue factory;
        std::string e;
        bool okF = core::parseJson(liteFactoryBlueprintJson(), factory, e), okD = false, okC = false;
        const std::string a = canonicalBlueprint(d.blueprints[0], okD), b = okF ? canonicalBlueprint(factory, okC) : std::string();
        if (!okD || !okC || a != b) issue("custom blueprint/topology (graph editing) is a full-edition feature");
    }
    if (!emptyPayload(d.timeline)) issue("lifecycle timeline is a full-edition feature");
    if (!emptyPayload(d.faults)) issue("fault overlays are a full-edition feature");
    static const std::set<std::string> liteFeatures = {"CD-F05", "CD-F06", "CD-F21", "CD-F22", "CD-F23", "CD-F25"};
    for (const auto& f : d.requiredFeatures) if (!liteFeatures.count(f)) issue("requires feature " + f + " (not in LITE)");
    std::map<std::string, double> params;
    for (const auto& [id, v] : d.baseParameters) params[id] = v;
    auto get = [&](const std::string& id) {
        const auto it = params.find(id);
        const core::ParamSpec* spec = core::findById(id.c_str());
        return it != params.end() ? it->second : (spec ? spec->def : 0.0);
    };
    for (const auto& [id, fixed] : liteFixedParams())
        if (get(id) != fixed) issue(std::string(id) + " = " + std::to_string(get(id)) + " (LITE fixes " + std::to_string(fixed) + ")");
    for (const auto& [id, v] : params) {
        bool known = false;
        for (const auto& fp : liteFixedParams()) known = known || id == fp.first;
        for (const char* m : kMacroIds) known = known || id == m;
        const core::ParamSpec* spec = core::findById(id.c_str());
        if (!known && (!spec || v != spec->def)) issue("parameter " + id + " is not available in LITE");
    }
    double m[4] = {0, 0, 0, 0};
    std::string e;
    if (!sceneMacros(d, params, m, e)) {
        issue(e);
    } else {
        if (m[3] != kLiteWarmth) issue("Warmth " + std::to_string(m[3]) + " (LITE fixes 0.2)");
        if (m[2] != kLiteDriftPerAge * m[0]) issue("Drift is not 0.25 x Age (independent Drift is a full-edition control)");
    }
    r.compatible = r.issues.empty();
    if (r.compatible) {
        LiteState s;
        s.knobs = {m[1], m[0]};
        s.seed = d.seed;
        out = std::move(s);
    }
    return r;
}

}  // namespace cd::edition
