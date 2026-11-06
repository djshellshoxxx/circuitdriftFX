#include "blueprint/BlueprintGraph.h"
#include <algorithm>
#include <cmath>
#include <set>

namespace cd::blueprint {
namespace {

using core::JsonValue;

std::string lower(std::string_view s) {
    std::string o(s);
    for (char& c : o) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return o;
}

// Keys that would make a blueprint executable or reference an external resource.
bool executableKey(const std::string& key) {
    static const std::set<std::string> deny = {
        "script", "scripts", "shader", "shaders", "url", "uri", "href", "path", "file", "filename", "plugin",
        "plugins", "library", "lib", "dll", "so", "spice", "netlist", "include", "import", "exec", "command",
        "code", "wasm", "module", "eval", "source", "src", "directive", "directives", "model_file", "modelfile"};
    return deny.count(lower(key)) != 0;
}

// True for strings that look like a filesystem path, URL or SPICE directive.
bool resourceLike(std::string_view s) {
    const std::string l = lower(s);
    if (l.find("://") != std::string::npos || l.find('\\') != std::string::npos) return true;
    for (const char* p : {"file:", "data:", "javascript:", "http:", "https:", "ftp:", "~"})
        if (l.rfind(p, 0) == 0) return true;
    if (!l.empty() && l[0] == '/') return true;
    if (l.size() >= 3 && l[1] == ':' && (l[2] == '/' || l[2] == '\\')) return true;
    if (l.find("../") != std::string::npos || l == "..") return true;
    for (const char* d : {".include", ".inc", ".lib", ".model", ".subckt", ".param", ".control", ".end", ".option", ".tran", ".ac "})
        if (l.rfind(d, 0) == 0) return true;
    return false;
}

struct Ctx {
    Diagnostics& d;
    void add(DiagCode c, std::string comp, std::string pin, std::string msg) { d.add(c, 1, std::move(comp), std::move(pin), std::move(msg)); }
};

// Walks the whole tree: executable keys and resource-like strings are rejected wherever they occur.
void scanTree(const JsonValue& v, Ctx& c, int depth, int& budget) {
    if (--budget < 0 || depth > kMaxDepth + 1) return;
    if (v.isString()) {
        if (resourceLike(v.string)) c.add(DiagCode::ResourceReference, "", "", "string looks like a path, URL or directive");
    } else if (v.isArray()) {
        for (const auto& e : v.array) scanTree(e, c, depth + 1, budget);
    } else if (v.isObject()) {
        for (const auto& kv : v.object) {
            if (executableKey(kv.first)) c.add(DiagCode::ExecutableField, "", "", "executable or resource field '" + kv.first.substr(0, 40) + "'");
            if (resourceLike(kv.first)) c.add(DiagCode::ResourceReference, "", "", "key looks like a path, URL or directive");
            scanTree(kv.second, c, depth + 1, budget);
        }
    }
}

bool onlyKeys(const JsonValue& o, std::initializer_list<const char*> allowed, Ctx& c, const std::string& comp, const char* where) {
    bool ok = true;
    for (const auto& kv : o.object) {
        bool known = false;
        for (const char* a : allowed) if (kv.first == a) known = true;
        if (!known && !executableKey(kv.first)) {  // executable keys are already reported by scanTree
            c.add(DiagCode::UnknownField, comp, "", std::string("unknown field in ") + where + ": " + kv.first.substr(0, 40));
            ok = false;
        }
    }
    return ok;
}

bool readId(const JsonValue* v, std::string& out, Ctx& c, const std::string& comp, const char* what) {
    if (!v || !v->isString()) { c.add(DiagCode::BadType, comp, "", std::string(what) + " must be a string"); return false; }
    if (!validIdentifier(v->string)) { c.add(DiagCode::BadId, comp, "", std::string(what) + " is not a valid identifier (1..64 of A-Za-z0-9_.-)"); return false; }
    out = v->string;
    return true;
}

void parseComponent(const JsonValue& j, BpComponent& comp, Ctx& c, int& pinTotal) {
    if (!j.isObject()) { c.add(DiagCode::BadType, "", "", "component must be an object"); return; }
    std::string tag;
    if (const auto* id = j.get("id"); id && id->isString()) tag = id->string.substr(0, 64);
    onlyKeys(j, {"id", "modelId", "pins", "parameters", "position"}, c, tag, "component");
    readId(j.get("id"), comp.id, c, tag, "component id");
    readId(j.get("modelId"), comp.modelId, c, tag, "modelId");
    const JsonValue* pins = j.get("pins");
    if (!pins || !pins->isObject()) {
        c.add(DiagCode::BadType, tag, "", "pins must be an object");
    } else {
        if (pins->object.size() < 2 || pins->object.size() > std::size_t(kMaxPinsPerComponent))
            c.add(DiagCode::LimitExceeded, tag, "", "pins must have 2..8 entries");
        for (const auto& kv : pins->object) {
            ++pinTotal;
            std::string node;
            if (!validIdentifier(kv.first)) { c.add(DiagCode::BadId, tag, kv.first.substr(0, 64), "invalid pin name"); continue; }
            if (!kv.second.isString() || !validIdentifier(kv.second.string)) {
                c.add(DiagCode::BadId, tag, kv.first, "pin node is not a valid identifier");
                continue;
            }
            if (comp.pins.size() < std::size_t(kMaxPinsPerComponent)) comp.pins.emplace_back(kv.first, kv.second.string);
        }
    }
    const JsonValue* ps = j.get("parameters");
    if (!ps || !ps->isObject()) {
        c.add(DiagCode::BadType, tag, "", "parameters must be an object");
    } else {
        if (ps->object.size() > std::size_t(kMaxParamsPerComponent)) c.add(DiagCode::LimitExceeded, tag, "", "more than 32 parameters");
        for (const auto& kv : ps->object) {
            if (!validIdentifier(kv.first)) { c.add(DiagCode::BadId, tag, "", "invalid parameter name"); continue; }
            if (!kv.second.isNumber() || !std::isfinite(kv.second.number)) { c.add(DiagCode::BadType, tag, "", "parameter '" + kv.first + "' must be a finite number"); continue; }
            if (comp.params.size() < std::size_t(kMaxParamsPerComponent)) comp.params.emplace_back(kv.first, kv.second.number);
        }
    }
    if (const JsonValue* pos = j.get("position")) {
        bool ok = pos->isArray() && pos->array.size() == 3;
        if (ok) for (const auto& e : pos->array) ok = ok && e.isNumber() && std::isfinite(e.number) && std::fabs(e.number) <= 1000.0;
        if (!ok) {
            c.add(DiagCode::BadType, tag, "", "position must be 3 numbers within +-1000");
        } else {
            comp.hasPosition = true;
            for (int k = 0; k < 3; ++k) comp.position[k] = pos->array[k].number;
        }
    }
}

bool parseTree(const JsonValue& v, Blueprint& out, Diagnostics& diag) {
    Ctx c{diag};
    const std::size_t before = diag.items.size();
    Blueprint bp;
    if (!v.isObject()) { c.add(DiagCode::BadType, "", "", "blueprint must be an object"); return false; }
    int budget = 200000;
    scanTree(v, c, 0, budget);
    if (budget < 0) c.add(DiagCode::LimitExceeded, "", "", "blueprint tree too large to scan");

    onlyKeys(v, {"schemaVersion", "id", "name", "modelRegistryVersion", "nodes", "components", "ports", "macroBindings", "modSlots"},
             c, "", "blueprint");
    const JsonValue* sv = v.get("schemaVersion");
    if (!sv || !sv->isNumber() || !sv->numberIsExactInteger || sv->number < 1) {
        c.add(DiagCode::UnknownSchema, "", "", "schemaVersion missing or not a positive integer");
    } else if (sv->number != 1) {
        c.add(DiagCode::UnknownSchema, "", "", "unknown schema major version");
    }
    readId(v.get("id"), bp.id, c, "", "blueprint id");
    readId(v.get("modelRegistryVersion"), bp.modelRegistryVersion, c, "", "modelRegistryVersion");
    if (const JsonValue* n = v.get("name"); !n || !n->isString()) {
        c.add(DiagCode::BadType, "", "", "name must be a string");
    } else {
        bool ctrl = false;
        for (unsigned char ch : n->string) if (ch < 0x20 || ch == 0x7f) ctrl = true;
        if (n->string.empty() || utf8CodePoints(n->string) > std::size_t(kMaxNameCodePoints) || ctrl)
            c.add(DiagCode::BadName, "", "", "name must be 1..80 code points without control characters");
        else bp.name = n->string;
    }

    const JsonValue* nodes = v.get("nodes");
    if (!nodes || !nodes->isArray()) {
        c.add(DiagCode::BadType, "", "", "nodes must be an array");
    } else {
        if (nodes->array.size() < 2 || nodes->array.size() > std::size_t(kMaxNodes)) c.add(DiagCode::LimitExceeded, "", "", "nodes must have 2..33 entries");
        std::set<std::string> seen;
        for (const auto& e : nodes->array) {
            std::string id;
            if (!readId(&e, id, c, "", "node id")) continue;
            if (!seen.insert(id).second) { c.add(DiagCode::DuplicateId, "", "", "duplicate node id " + id); continue; }
            if (bp.nodes.size() < std::size_t(kMaxNodes)) bp.nodes.push_back(id);
        }
    }

    int pinTotal = 0;
    const JsonValue* comps = v.get("components");
    if (!comps || !comps->isArray()) {
        c.add(DiagCode::BadType, "", "", "components must be an array");
    } else {
        if (comps->array.empty() || comps->array.size() > std::size_t(kMaxComponents)) c.add(DiagCode::LimitExceeded, "", "", "components must have 1..64 entries");
        std::set<std::string> seen;
        for (const auto& e : comps->array) {
            if (bp.components.size() >= std::size_t(kMaxComponents)) break;
            BpComponent comp;
            parseComponent(e, comp, c, pinTotal);
            if (!comp.id.empty() && !seen.insert(comp.id).second) c.add(DiagCode::DuplicateId, comp.id, "", "duplicate component id");
            bp.components.push_back(std::move(comp));
        }
        if (pinTotal > kMaxPinConnections) c.add(DiagCode::LimitExceeded, "", "", "more than 256 pin connections");
        if (int(comps->array.size()) > kMaxThermalNodes) c.add(DiagCode::LimitExceeded, "", "", "more than 64 thermal nodes");
    }

    if (const JsonValue* ports = v.get("ports"); !ports || !ports->isObject()) {
        c.add(DiagCode::BadType, "", "", "ports must be an object");
    } else {
        onlyKeys(*ports, {"input", "output", "ground"}, c, "", "ports");
        readId(ports->get("input"), bp.input, c, "", "input port");
        readId(ports->get("output"), bp.output, c, "", "output port");
        const JsonValue* g = ports->get("ground");
        if (!g || !g->isString() || g->string != "0") c.add(DiagCode::NoGround, "", "", "ports.ground must be \"0\"");
    }

    if (const JsonValue* mb = v.get("macroBindings"); !mb || !mb->isArray()) {
        c.add(DiagCode::BadType, "", "", "macroBindings must be an array");
    } else {
        if (mb->array.size() > std::size_t(kMaxMacroBindings)) c.add(DiagCode::LimitExceeded, "", "", "more than 64 macro bindings");
        for (const auto& e : mb->array) {
            if (bp.macroBindings.size() >= std::size_t(kMaxMacroBindings)) break;
            if (!e.isObject()) { c.add(DiagCode::BadType, "", "", "macro binding must be an object"); continue; }
            onlyKeys(e, {"macro", "component", "field"}, c, "", "macro binding");
            MacroBinding b;
            const JsonValue* m = e.get("macro");
            if (!m || !m->isString() || !(m->string == "age" || m->string == "sag" || m->string == "drift" || m->string == "warmth"))
                c.add(DiagCode::BadType, "", "", "macro must be age|sag|drift|warmth");
            else b.macro = m->string;
            readId(e.get("component"), b.component, c, "", "macro component");
            readId(e.get("field"), b.field, c, "", "macro field");
            bp.macroBindings.push_back(std::move(b));
        }
    }

    if (const JsonValue* ms = v.get("modSlots")) {
        if (!ms->isArray() || ms->array.size() > std::size_t(kModSlotCount)) {
            c.add(DiagCode::BadType, "", "", "modSlots must be an array of at most 16 bindings");
        } else {
            std::set<int> slots;
            for (const auto& e : ms->array) {
                if (!e.isObject()) { c.add(DiagCode::BadType, "", "", "mod slot binding must be an object"); continue; }
                onlyKeys(e, {"slot", "component", "field"}, c, "", "mod slot binding");
                ModSlotBinding b;
                const JsonValue* s = e.get("slot");
                if (!s || !s->isNumber() || !s->numberIsExactInteger || s->number < 0 || s->number >= kModSlotCount) {
                    c.add(DiagCode::BadBinding, "", "", "mod slot index must be an integer 0..15");
                    continue;
                }
                b.slot = int(s->number);
                if (!slots.insert(b.slot).second) c.add(DiagCode::DuplicateId, "", "", "duplicate mod slot " + std::to_string(b.slot));
                readId(e.get("component"), b.component, c, "", "mod slot component");
                readId(e.get("field"), b.field, c, "", "mod slot field");
                bp.modSlots.push_back(std::move(b));
            }
        }
    }

    if (diag.items.size() != before || diag.truncated) return false;
    out = std::move(bp);
    return true;
}

}  // namespace

const char* diagCodeName(DiagCode c) noexcept {
    static const char* const names[] = {
        "Malformed", "TooLarge", "TooDeep", "DuplicateKey", "NonFinite", "UnknownSchema", "UnknownField", "ExecutableField",
        "ResourceReference", "BadType", "BadId", "BadName", "DuplicateId", "LimitExceeded",
        "RegistryMismatch", "UnsupportedModel", "PinMissing", "PinUnknown", "PinNode", "ParamMissing", "ParamUnknown",
        "ParamRange", "ConductanceUnbounded", "BadBinding",
        "NoGround", "BadTerminal", "SelfShort", "FloatingNode", "FloatingIsland", "NoDcPath", "VoltageSourceLoop", "SupplyShort",
        "BadConfig", "UnknownCount", "DcFailed", "ProbeFailed", "Internal"};
    const auto k = std::size_t(c);
    return k < sizeof(names) / sizeof(names[0]) ? names[k] : "Unknown";
}

void Diagnostics::add(DiagCode code, int check, std::string component, std::string pin, std::string message) {
    if (items.size() >= kMax) { truncated = true; return; }
    items.push_back({code, check, std::move(component), std::move(pin), std::move(message)});
}
bool Diagnostics::has(DiagCode c) const noexcept {
    for (const auto& d : items) if (d.code == c) return true;
    return false;
}
bool Diagnostics::hasFor(DiagCode c, std::string_view component) const noexcept {
    for (const auto& d : items) if (d.code == c && d.component == component) return true;
    return false;
}

const std::string* BpComponent::pinNode(std::string_view pin) const noexcept {
    for (const auto& p : pins) if (p.first == pin) return &p.second;
    return nullptr;
}
const double* BpComponent::param(std::string_view name) const noexcept {
    for (const auto& p : params) if (p.first == name) return &p.second;
    return nullptr;
}
int Blueprint::componentIndex(std::string_view cid) const noexcept {
    for (std::size_t k = 0; k < components.size(); ++k) if (components[k].id == cid) return int(k);
    return -1;
}
int Blueprint::pinConnectionCount() const noexcept {
    int n = 0;
    for (const auto& c : components) n += int(c.pins.size());
    return n;
}

bool validIdentifier(std::string_view s) noexcept {
    if (s.empty() || s.size() > std::size_t(kMaxIdChars)) return false;
    bool allDots = true;
    for (char ch : s) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == '-';
        if (!ok) return false;
        if (ch != '.') allDots = false;
    }
    return !allDots;
}

std::size_t utf8CodePoints(std::string_view s) noexcept {
    std::size_t n = 0;
    for (unsigned char ch : s) if ((ch & 0xC0) != 0x80) ++n;
    return n;
}

bool parseBlueprint(std::string_view text, Blueprint& out, Diagnostics& diag) {
    if (text.size() > kMaxBytes) { diag.add(DiagCode::TooLarge, 1, "", "", "blueprint exceeds 1 MiB"); return false; }
    JsonValue v;
    std::string err;
    core::JsonLimits lim;
    lim.maxBytes = kMaxBytes;
    lim.maxDepth = kMaxDepth;
    if (!core::parseJson(text, v, err, lim)) {
        DiagCode code = DiagCode::Malformed;
        if (err.find("duplicate key") != std::string::npos) code = DiagCode::DuplicateKey;
        else if (err.find("nesting too deep") != std::string::npos) code = DiagCode::TooDeep;
        else if (err.find("finite") != std::string::npos) code = DiagCode::NonFinite;
        else if (err.find("size bound") != std::string::npos) code = DiagCode::TooLarge;
        diag.add(code, 1, "", "", "JSON: " + err);
        return false;
    }
    return parseTree(v, out, diag);
}

bool parseBlueprint(const core::JsonValue& v, Blueprint& out, Diagnostics& diag) {
    if (core::jsonDepth(v) > kMaxDepth) { diag.add(DiagCode::TooDeep, 1, "", "", "nesting deeper than 16"); return false; }
    try {
        if (core::writeJson(v).size() > kMaxBytes) { diag.add(DiagCode::TooLarge, 1, "", "", "blueprint exceeds 1 MiB"); return false; }
    } catch (const std::exception&) {
        diag.add(DiagCode::NonFinite, 1, "", "", "non-finite number in tree");
        return false;
    }
    return parseTree(v, out, diag);
}

std::string writeBlueprint(const Blueprint& bp) {
    JsonValue o = JsonValue::makeObject();
    o.set("schemaVersion", JsonValue::makeNumber(bp.schemaVersion));
    o.set("id", JsonValue::makeString(bp.id));
    o.set("name", JsonValue::makeString(bp.name));
    o.set("modelRegistryVersion", JsonValue::makeString(bp.modelRegistryVersion));
    JsonValue nodes = JsonValue::makeArray();
    for (const auto& n : bp.nodes) nodes.array.push_back(JsonValue::makeString(n));
    o.set("nodes", std::move(nodes));
    JsonValue comps = JsonValue::makeArray();
    for (const auto& c : bp.components) {
        JsonValue j = JsonValue::makeObject();
        j.set("id", JsonValue::makeString(c.id));
        j.set("modelId", JsonValue::makeString(c.modelId));
        JsonValue pins = JsonValue::makeObject();
        for (const auto& p : c.pins) pins.set(p.first, JsonValue::makeString(p.second));
        j.set("pins", std::move(pins));
        JsonValue ps = JsonValue::makeObject();
        for (const auto& p : c.params) ps.set(p.first, JsonValue::makeNumber(p.second));
        j.set("parameters", std::move(ps));
        if (c.hasPosition) {
            JsonValue pos = JsonValue::makeArray();
            for (double d : c.position) pos.array.push_back(JsonValue::makeNumber(d));
            j.set("position", std::move(pos));
        }
        comps.array.push_back(std::move(j));
    }
    o.set("components", std::move(comps));
    JsonValue ports = JsonValue::makeObject();
    ports.set("input", JsonValue::makeString(bp.input));
    ports.set("output", JsonValue::makeString(bp.output));
    ports.set("ground", JsonValue::makeString("0"));
    o.set("ports", std::move(ports));
    JsonValue mb = JsonValue::makeArray();
    for (const auto& m : bp.macroBindings) {
        JsonValue j = JsonValue::makeObject();
        j.set("macro", JsonValue::makeString(m.macro));
        j.set("component", JsonValue::makeString(m.component));
        j.set("field", JsonValue::makeString(m.field));
        mb.array.push_back(std::move(j));
    }
    o.set("macroBindings", std::move(mb));
    if (!bp.modSlots.empty()) {
        JsonValue ms = JsonValue::makeArray();
        for (const auto& m : bp.modSlots) {
            JsonValue j = JsonValue::makeObject();
            j.set("slot", JsonValue::makeNumber(m.slot));
            j.set("component", JsonValue::makeString(m.component));
            j.set("field", JsonValue::makeString(m.field));
            ms.array.push_back(std::move(j));
        }
        o.set("modSlots", std::move(ms));
    }
    return core::writeJson(o);
}

}  // namespace cd::blueprint
