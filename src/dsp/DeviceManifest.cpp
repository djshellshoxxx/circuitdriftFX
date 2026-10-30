#include "dsp/DeviceManifest.h"
#include <cmath>
#include <set>

namespace cd::dsp {
namespace {

struct Family {
    const char* name;
    DeviceEquation eq;
    int pinCount;
    std::vector<std::pair<const char*, const char*>> params;  // required name, unit (in order)
    bool needsDomain;
};

const std::vector<Family>& families() {
    static const std::vector<Family> f = {
        {"resistor", DeviceEquation::Resistor, 2, {{"resistanceOhm", "Ohm"}}, false},
        {"capacitor", DeviceEquation::Capacitor, 2, {{"capacitanceF", "F"}}, false},
        {"shockley-diode", DeviceEquation::ShockleyDiode, 2,
         {{"saturationCurrentA", "A"}, {"emissionCoefficient", "1"}, {"thermalVoltageV", "V"}}, true},
    };
    return f;
}

// SYNTHETIC FIXTURES: round illustrative numbers, no device, datasheet or measurement behind them.
const char* const kFixtures[kSyntheticFixtureCount] = {
    R"({"schemaVersion":1,"modelId":"fixture.resistor","revision":1,"equation":"resistor","pins":["p","n"],
        "parameters":[{"name":"resistanceOhm","unit":"Ohm","min":1e-3,"max":1e9,"default":10000}],
        "scales":{"voltageV":1,"currentA":1e-3},
        "provenance":{"kind":"synthetic-fixture","note":"Ideal linear resistor for solver validation; not a measured component."}})",
    R"({"schemaVersion":1,"modelId":"fixture.capacitor","revision":1,"equation":"capacitor","pins":["p","n"],
        "parameters":[{"name":"capacitanceF","unit":"F","min":1e-15,"max":1,"default":1e-8}],
        "scales":{"voltageV":1,"currentA":1e-3},
        "provenance":{"kind":"synthetic-fixture","note":"Ideal linear capacitor q=C*v for solver validation; not a measured component."}})",
    R"({"schemaVersion":1,"modelId":"fixture.diode","revision":1,"equation":"shockley-diode","pins":["anode","cathode"],
        "parameters":[{"name":"saturationCurrentA","unit":"A","min":1e-18,"max":1e-6,"default":1e-14},
                      {"name":"emissionCoefficient","unit":"1","min":0.5,"max":4,"default":1},
                      {"name":"thermalVoltageV","unit":"V","min":0.02,"max":0.04,"default":0.025852}],
        "scales":{"voltageV":1,"currentA":1e-3},"domain":{"maxExpArg":40},
        "provenance":{"kind":"synthetic-fixture","note":"Textbook Shockley equation with arbitrary round parameters; not a named or measured device (G01 open)."}})",
};

bool positive(const core::JsonValue* v) { return v && v->isNumber() && v->number > 0; }

}  // namespace

int DeviceManifest::paramIndex(const std::string& n) const noexcept {
    for (std::size_t k = 0; k < params.size(); ++k) if (params[k].name == n) return int(k);
    return -1;
}

bool parseDeviceManifest(const core::JsonValue& v, DeviceManifest& m, std::string& err) {
    auto bad = [&](std::string why) { err = "manifest: " + why; return false; };
    if (!v.isObject()) return bad("not an object");
    static const std::set<std::string> allowed = {"schemaVersion", "modelId", "revision", "equation", "pins",
                                                  "parameters", "scales", "domain", "provenance"};
    for (const auto& kv : v.object) if (!allowed.count(kv.first)) return bad("unknown key " + kv.first);
    const auto* sv = v.get("schemaVersion");
    if (!sv || !sv->isNumber() || sv->number != 1) return bad("schemaVersion must be 1");
    const auto* id = v.get("modelId");
    if (!id || !id->isString() || id->string.empty() || id->string.size() > 64) return bad("modelId");
    const auto* rev = v.get("revision");
    if (!rev || !rev->isNumber() || rev->number < 1 || rev->number != std::floor(rev->number) || rev->number > 1e6)
        return bad("revision");
    const auto* eq = v.get("equation");
    const Family* fam = nullptr;
    if (eq && eq->isString()) for (const auto& f : families()) if (eq->string == f.name) fam = &f;
    if (!fam) return bad("unknown equation family");
    m = DeviceManifest{};
    m.modelId = id->string;
    m.revision = int(rev->number);
    m.equation = fam->eq;

    const auto* pins = v.get("pins");
    if (!pins || !pins->isArray() || int(pins->array.size()) != fam->pinCount) return bad("pin count");
    std::set<std::string> pinSet;
    for (const auto& p : pins->array) {
        if (!p.isString() || p.string.empty() || !pinSet.insert(p.string).second) return bad("pin names");
        m.pins.push_back(p.string);
    }

    const auto* ps = v.get("parameters");
    if (!ps || !ps->isArray() || ps->array.size() != fam->params.size()) return bad("parameter count");
    for (std::size_t k = 0; k < ps->array.size(); ++k) {
        const auto& p = ps->array[k];
        if (!p.isObject() || p.object.size() != 5) return bad("parameter shape");
        const auto *n = p.get("name"), *u = p.get("unit"), *lo = p.get("min"), *hi = p.get("max"), *d = p.get("default");
        if (!n || !n->isString() || n->string != fam->params[k].first) return bad("parameter name/order");
        if (!u || !u->isString() || u->string != fam->params[k].second) return bad("unit of " + n->string);
        if (!lo || !hi || !d || !lo->isNumber() || !hi->isNumber() || !d->isNumber()) return bad("bounds of " + n->string);
        if (!(lo->number <= d->number && d->number <= hi->number)) return bad("default outside bounds: " + n->string);
        if (lo->number <= 0) return bad("non-positive lower bound: " + n->string);  // all families need >0
        m.params.push_back({n->string, u->string, lo->number, hi->number, d->number});
    }

    const auto* sc = v.get("scales");
    if (!sc || !sc->isObject() || sc->object.size() != 2 || !positive(sc->get("voltageV")) || !positive(sc->get("currentA")))
        return bad("scales must be two strictly positive values");
    m.voltageScaleV = sc->get("voltageV")->number;
    m.currentScaleA = sc->get("currentA")->number;

    const auto* dom = v.get("domain");
    if (fam->needsDomain) {
        if (!dom || !dom->isObject() || dom->object.size() != 1) return bad("domain required");
        const auto* e = dom->get("maxExpArg");
        if (!positive(e) || e->number > 80) return bad("maxExpArg must be in (0,80]");
        m.maxExpArg = e->number;
    } else if (dom) {
        return bad("domain not applicable");
    }

    const auto* pv = v.get("provenance");
    if (!pv || !pv->isObject()) return bad("provenance required");
    const auto *kind = pv->get("kind"), *note = pv->get("note"), *src = pv->get("source");
    for (const auto& kv : pv->object)
        if (kv.first != "kind" && kv.first != "note" && kv.first != "source") return bad("unknown provenance key");
    if (!note || !note->isString() || note->string.empty()) return bad("provenance note");
    m.provenanceNote = note->string;
    if (kind && kind->isString() && kind->string == "synthetic-fixture") {
        m.provenance = ProvenanceKind::SyntheticFixture;
    } else if (kind && kind->isString() && kind->string == "measured") {
        if (!src || !src->isString() || src->string.empty()) return bad("measured provenance needs source");
        m.provenance = ProvenanceKind::Measured;
        m.provenanceSource = src->string;
    } else {
        return bad("provenance kind");
    }
    err.clear();
    return true;
}

const char* syntheticFixtureJson(int index) noexcept {
    return (index >= 0 && index < kSyntheticFixtureCount) ? kFixtures[index] : nullptr;
}

const std::vector<DeviceManifest>& syntheticFixtureCatalogue() {
    static const std::vector<DeviceManifest> cat = [] {
        std::vector<DeviceManifest> c;
        for (int k = 0; k < kSyntheticFixtureCount; ++k) {
            core::JsonValue j;
            std::string e;
            DeviceManifest m;
            if (core::parseJson(kFixtures[k], j, e) && parseDeviceManifest(j, m, e)) c.push_back(std::move(m));
        }
        return c;
    }();
    return cat;
}

const DeviceManifest* findSyntheticFixture(const std::string& id) {
    for (const auto& m : syntheticFixtureCatalogue()) if (m.modelId == id) return &m;
    return nullptr;
}

}  // namespace cd::dsp
