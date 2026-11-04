#include "blueprint/Compiler.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>

namespace cd::blueprint {
namespace {

using dsp::DeviceEquation;
using dsp::DeviceManifest;
using dsp::NetElement;

constexpr double kPi = 3.14159265358979323846;
constexpr double kProbeBoundVolts = 1000.0;   // finite-bounded probe output limit
constexpr double kProbeSeconds = 0.25;
constexpr double kShortOhm = 1.0;             // resistor below this directly across a source is a supply short

struct Resolved {
    const DeviceManifest* dev = nullptr;
    const SourceModel* src = nullptr;
    int a = 0, b = 0;                         // netlist node indices (0 = ground)
    double param[3] = {0, 0, 0};
    bool valid = false;
    bool isSource() const { return src != nullptr; }
    bool conductive() const { return src || (dev && dev->equation != DeviceEquation::Capacitor); }
};

struct Dsu {
    std::vector<int> p;
    explicit Dsu(int n) : p(std::size_t(n)) { std::iota(p.begin(), p.end(), 0); }
    int find(int i) { while (p[std::size_t(i)] != i) i = p[std::size_t(i)] = p[std::size_t(p[std::size_t(i)])]; return i; }
    bool unite(int a, int b) { a = find(a); b = find(b); if (a == b) return false; p[std::size_t(a)] = b; return true; }
};

bool validConfig(const RuntimeConfig& c) {
    const int os = c.oversampling;
    return std::isfinite(c.sampleRate) && c.sampleRate >= 8000 && c.sampleRate <= 384000 &&
           (os == 1 || os == 2 || os == 4 || os == 8 || os == 16) &&
           std::isfinite(c.fullScaleVolts) && c.fullScaleVolts > 0 && c.fullScaleVolts <= 100;
}

CompileResult fail(CompileResult r, int check) {
    r.ok = false;
    r.graph.reset();
    r.failedCheck = check;
    return r;
}

// Names of the fields a model exposes as parameters, in manifest order.
std::vector<std::string> fieldNames(const DeviceManifest* dev, const SourceModel* src) {
    std::vector<std::string> f;
    if (dev) for (const auto& p : dev->params) f.push_back(p.name);
    if (src) f.push_back(src->paramName);
    return f;
}

void resolveBinding(const Blueprint& bp, const std::vector<Resolved>& rs, const std::string& comp, const std::string& field,
                    const char* what, Diagnostics& d) {
    const int ci = bp.componentIndex(comp);
    if (ci < 0) { d.add(DiagCode::BadBinding, 2, comp, "", std::string(what) + " names an unknown component"); return; }
    const Resolved& r = rs[std::size_t(ci)];
    if (!r.valid) return;  // component already reported
    const auto names = fieldNames(r.dev, r.src);
    if (std::find(names.begin(), names.end(), field) == names.end())
        d.add(DiagCode::BadBinding, 2, comp, "", std::string(what) + " names unknown field '" + field.substr(0, 40) + "'");
}

// Checks 2 and the node resolution they need. Returns false when diagnostics were produced.
bool checkModels(const Blueprint& bp, const ModelRegistry& reg, const std::map<std::string, int>& nodeIdx,
                 std::vector<Resolved>& rs, Diagnostics& d) {
    const std::size_t before = d.items.size();
    if (bp.modelRegistryVersion != reg.version)
        d.add(DiagCode::RegistryMismatch, 2, "", "", "blueprint targets model registry '" + bp.modelRegistryVersion.substr(0, 64) + "'");
    rs.assign(bp.components.size(), Resolved{});
    for (std::size_t k = 0; k < bp.components.size(); ++k) {
        const BpComponent& c = bp.components[k];
        Resolved& r = rs[k];
        r.dev = reg.findDevice(c.modelId);
        r.src = r.dev ? nullptr : reg.findSource(c.modelId);
        if (!r.dev && !r.src) { d.add(DiagCode::UnsupportedModel, 2, c.id, "", "model '" + c.modelId + "' is not in the registry"); continue; }
        const std::vector<std::string> pins = r.dev ? r.dev->pins : std::vector<std::string>{"p", "n"};
        bool pinsOk = true;
        for (const auto& pn : pins)
            if (!c.pinNode(pn)) { d.add(DiagCode::PinMissing, 2, c.id, pn, "required pin is not connected"); pinsOk = false; }
        for (const auto& pr : c.pins)
            if (std::find(pins.begin(), pins.end(), pr.first) == pins.end()) {
                d.add(DiagCode::PinUnknown, 2, c.id, pr.first, "model has no such pin (pins are never guessed)");
                pinsOk = false;
            }
        int nodeOf[2] = {0, 0};
        for (std::size_t pi = 0; pinsOk && pi < pins.size(); ++pi) {
            const auto it = nodeIdx.find(*c.pinNode(pins[pi]));
            if (it == nodeIdx.end()) { d.add(DiagCode::PinNode, 2, c.id, pins[pi], "pin refers to an undeclared node"); pinsOk = false; }
            else nodeOf[pi] = it->second;
        }
        const auto names = fieldNames(r.dev, r.src);
        bool paramsOk = true;
        for (std::size_t pi = 0; pi < names.size(); ++pi) {
            const double* v = c.param(names[pi]);
            if (!v) { d.add(DiagCode::ParamMissing, 2, c.id, "", "parameter '" + names[pi] + "' missing"); paramsOk = false; continue; }
            const double lo = r.dev ? r.dev->params[pi].min : r.src->min;
            const double hi = r.dev ? r.dev->params[pi].max : r.src->max;
            if (!std::isfinite(*v) || *v < lo || *v > hi) { d.add(DiagCode::ParamRange, 2, c.id, "", "parameter '" + names[pi] + "' outside manifest range"); paramsOk = false; continue; }
            if (pi < 3) r.param[pi] = *v;
        }
        for (const auto& pr : c.params)
            if (std::find(names.begin(), names.end(), pr.first) == names.end()) {
                d.add(DiagCode::ParamUnknown, 2, c.id, "", "model has no parameter '" + pr.first.substr(0, 40) + "'");
                paramsOk = false;
            }
        if (paramsOk && r.dev && r.dev->equation == DeviceEquation::Resistor && !std::isfinite(1.0 / r.param[0])) {
            d.add(DiagCode::ConductanceUnbounded, 2, c.id, "", "conductance is not finite");
            paramsOk = false;
        }
        r.a = nodeOf[0];
        r.b = nodeOf[1];
        r.valid = pinsOk && paramsOk;
    }
    for (const auto& m : bp.macroBindings) resolveBinding(bp, rs, m.component, m.field, "macro binding", d);
    for (const auto& m : bp.modSlots) resolveBinding(bp, rs, m.component, m.field, "mod slot binding", d);
    return d.items.size() == before;
}

// Check 3: connectivity, ground reference, terminals, floating nodes/islands, V-source loops, shorts.
bool checkConnectivity(const Blueprint& bp, const std::map<std::string, int>& nodeIdx, const std::vector<Resolved>& rs,
                       Diagnostics& d) {
    const std::size_t before = d.items.size();
    const int n = int(nodeIdx.size());  // includes ground
    const auto gi = nodeIdx.find("0");
    if (gi == nodeIdx.end()) { d.add(DiagCode::NoGround, 3, "", "", "node \"0\" is not declared"); return false; }
    std::vector<int> pinCount(std::size_t(n), 0);
    for (const auto& r : rs) { ++pinCount[std::size_t(r.a)]; ++pinCount[std::size_t(r.b)]; }
    if (pinCount[0] == 0) d.add(DiagCode::NoGround, 3, "", "", "no component pin is connected to ground \"0\"");

    const auto in = nodeIdx.find(bp.input), out = nodeIdx.find(bp.output);
    if (in == nodeIdx.end()) d.add(DiagCode::BadTerminal, 3, "", "", "input terminal is not a declared node");
    if (out == nodeIdx.end()) d.add(DiagCode::BadTerminal, 3, "", "", "output terminal is not a declared node");
    if (in != nodeIdx.end() && out != nodeIdx.end()) {
        if (in->second == 0 || out->second == 0) d.add(DiagCode::BadTerminal, 3, "", "", "input/output terminal cannot be ground");
        else if (in->second == out->second) d.add(DiagCode::BadTerminal, 3, "", "", "input and output terminals must differ");
        if (in->second != 0 && pinCount[std::size_t(in->second)] == 0) d.add(DiagCode::BadTerminal, 3, "", "", "input terminal has no connected pin");
        if (out->second != 0 && pinCount[std::size_t(out->second)] == 0) d.add(DiagCode::BadTerminal, 3, "", "", "output terminal has no connected pin");
    }

    std::vector<std::string> names(static_cast<std::size_t>(n), std::string());
    for (const auto& kv : nodeIdx) names[std::size_t(kv.second)] = kv.first;

    Dsu structural(n), dc(n), vloop(n);
    const bool haveIn = in != nodeIdx.end() && in->second != 0;
    if (haveIn) { dc.unite(in->second, 0); vloop.unite(in->second, 0); }  // injected drive source
    for (std::size_t k = 0; k < rs.size(); ++k) {
        const Resolved& r = rs[k];
        const BpComponent& c = bp.components[k];
        if (r.a == r.b) { d.add(DiagCode::SelfShort, 3, c.id, "", "both pins are on the same node"); continue; }
        structural.unite(r.a, r.b);
        if (r.conductive()) dc.unite(r.a, r.b);
        if (r.isSource() && !vloop.unite(r.a, r.b))
            d.add(DiagCode::VoltageSourceLoop, 3, c.id, "", "ideal voltage sources form a loop");
    }
    for (std::size_t k = 0; k < rs.size(); ++k) {   // low-ohm resistor directly across a source
        const Resolved& r = rs[k];
        if (!r.dev || r.dev->equation != DeviceEquation::Resistor || r.param[0] >= kShortOhm || r.a == r.b) continue;
        for (std::size_t j = 0; j < rs.size(); ++j) {
            const Resolved& s = rs[j];
            if (!s.isSource() || s.a == s.b) continue;
            if ((s.a == r.a && s.b == r.b) || (s.a == r.b && s.b == r.a))
                d.add(DiagCode::SupplyShort, 3, bp.components[k].id, "", "resistor below 1 ohm directly across source " + bp.components[j].id);
        }
    }
    for (int i = 1; i < n; ++i) {
        if (pinCount[std::size_t(i)] == 0) { d.add(DiagCode::FloatingNode, 3, "", names[std::size_t(i)], "declared node has no connected pin"); continue; }
        if (structural.find(i) != structural.find(0)) d.add(DiagCode::FloatingIsland, 3, "", names[std::size_t(i)], "node is not connected to ground (island)");
        else if (dc.find(i) != dc.find(0)) d.add(DiagCode::NoDcPath, 3, "", names[std::size_t(i)], "node has no DC path to ground (capacitor-only coupling)");
    }
    return d.items.size() == before;
}

struct ProbeResult { bool ok = false; double peak = 0; int maxNewton = 0; std::string why; };

ProbeResult runProbe(const dsp::MnaSolver& proto, int inputElement, int outputNode, double rate, double fullScale, int kind) {
    dsp::MnaSolver s = proto;
    ProbeResult pr;
    const long n = long(std::llround(kProbeSeconds * rate));
    const double amp = kind == 1 ? std::pow(10.0, -18.0 / 20.0) : kind == 2 ? std::pow(10.0, -6.0 / 20.0) : 0.0;
    const long pulse = long(std::llround(0.005 * rate));
    double phase = 0;
    const double dphi = 2.0 * kPi * 1000.0 / rate;
    for (long i = 0; i < n; ++i) {
        double x = 0;
        if (kind == 1) { x = amp * std::sin(phase); phase += dphi; if (phase > 2 * kPi) phase -= 2 * kPi; }
        else if (kind == 2 && i < pulse) x = amp * std::sin(kPi * double(i) / double(pulse));
        s.setSource(inputElement, x * fullScale);
        if (!s.step()) { pr.why = "solver step failed during probe"; return pr; }
        pr.maxNewton = std::max(pr.maxNewton, s.lastIterations());
        const double v = s.voltage(outputNode);
        if (!std::isfinite(v) || std::fabs(v) > kProbeBoundVolts) { pr.why = "probe output not finite or beyond +-1000 V"; return pr; }
        for (int k = 0; k < s.unknowns(); ++k)
            if (!std::isfinite(s.unknown(k)) || std::fabs(s.unknown(k)) > 1e6) { pr.why = "probe state not finite/bounded"; return pr; }
        pr.peak = std::max(pr.peak, std::fabs(v));
    }
    if (s.needsFallback() || s.failures() != 0) { pr.why = "solver required emergency fallback during probe"; return pr; }
    pr.ok = true;
    return pr;
}

}  // namespace

const DeviceManifest* ModelRegistry::findDevice(const std::string& id) const noexcept {
    for (const auto& m : devices) if (m.modelId == id) return &m;
    return nullptr;
}
const SourceModel* ModelRegistry::findSource(const std::string& id) const noexcept {
    for (const auto& m : sources) if (m.modelId == id) return &m;
    return nullptr;
}
ModelRegistry ModelRegistry::syntheticFixtures() {
    ModelRegistry r;
    r.version = "fixture-v1";
    r.devices = dsp::syntheticFixtureCatalogue();
    r.sources.push_back({"fixture.vsource", 1, "voltageV", -100, 100});
    return r;
}

bool PreparedGraph::makeSolver(dsp::MnaSolver& out, std::string& error) const {
    return out.prepare(netlist, 1.0 / (config.sampleRate * config.oversampling), {}, error);
}

CompileResult compile(const Blueprint& bp, const ModelRegistry& reg, const RuntimeConfig& cfg, Generation generation) {
    CompileResult res;
    CostReport& cost = res.cost;
    Diagnostics& d = res.diagnostics;
    if (!validConfig(cfg)) {
        d.add(DiagCode::BadConfig, 4, "", "", "sample rate, oversampling or full-scale voltage outside supported range");
        return fail(std::move(res), 4);
    }
    // Check 1 re-asserted for programmatically built blueprints.
    if (bp.components.empty() || bp.components.size() > std::size_t(kMaxComponents) || bp.nodes.size() > std::size_t(kMaxNodes) ||
        bp.pinConnectionCount() > kMaxPinConnections || int(bp.components.size()) > kMaxThermalNodes) {
        d.add(DiagCode::LimitExceeded, 1, "", "", "blueprint exceeds component/node/pin limits");
        return fail(std::move(res), 1);
    }
    cost.components = int(bp.components.size());
    cost.nodes = int(bp.nodes.size());
    cost.pinConnections = bp.pinConnectionCount();
    cost.thermalNodes = cost.components;  // conservative: one thermal node per component, none declared by fixtures
    cost.sampleRate = cfg.sampleRate;
    cost.oversampling = cfg.oversampling;

    std::map<std::string, int> nodeIdx;
    nodeIdx["0"] = 0;
    {
        int next = 1;
        bool dup = false;
        std::set<std::string> seen;
        for (const auto& nd : bp.nodes) {
            dup = dup || !seen.insert(nd).second;
            if (nd != "0" && !nodeIdx.count(nd)) nodeIdx[nd] = next++;
        }
        if (dup) { d.add(DiagCode::DuplicateId, 1, "", "", "duplicate node id"); return fail(std::move(res), 1); }
        if (!seen.count("0")) nodeIdx.erase("0");  // undeclared ground is reported by check 3
    }

    // Check 2.
    std::vector<Resolved> rs;
    if (!checkModels(bp, reg, nodeIdx, rs, d)) return fail(std::move(res), 2);
    // Check 3.
    if (!checkConnectivity(bp, nodeIdx, rs, d)) return fail(std::move(res), 3);

    // Check 4: symbolic structure + numeric DC.
    auto graph = std::make_shared<PreparedGraph>();
    PreparedGraph& g = *graph;
    g.generation = generation;
    g.blueprint = bp;
    g.config = cfg;
    g.nodeNames.assign(nodeIdx.size(), std::string());
    for (const auto& kv : nodeIdx) g.nodeNames[std::size_t(kv.second)] = kv.first;
    g.models_.reserve(rs.size());
    std::map<std::string, std::size_t> modelSlot;
    int vs = 1, nonlinear = 0;   // vs counts the injected drive source
    double vScale = 0, iScale = 1e300;
    for (const auto& r : rs) {
        if (r.src) { ++vs; continue; }
        if (r.dev->equation == DeviceEquation::ShockleyDiode) ++nonlinear;
        if (!modelSlot.count(r.dev->modelId)) { modelSlot[r.dev->modelId] = g.models_.size(); g.models_.push_back(*r.dev); }
        vScale = std::max(vScale, r.dev->voltageScaleV);
        iScale = std::min(iScale, r.dev->currentScaleA);
    }
    cost.nonlinear = nonlinear;
    cost.voltageSources = vs;
    cost.unknowns = int(nodeIdx.size()) - 1 + vs;
    if (nonlinear > kMaxNonlinear) { d.add(DiagCode::LimitExceeded, 4, "", "", "more than 8 nonlinear devices"); return fail(std::move(res), 4); }
    if (cost.unknowns > kMaxUnknowns) { d.add(DiagCode::UnknownCount, 4, "", "", "more than 96 electrical unknowns"); return fail(std::move(res), 4); }
    if (int(rs.size()) + 1 > dsp::MnaSolver::kMaxComponents) {
        d.add(DiagCode::LimitExceeded, 4, "", "", "solver element budget exceeded (components plus the injected drive source > 64)");
        return fail(std::move(res), 4);
    }
    if (vScale <= 0) vScale = 1;
    if (iScale > 1e299) iScale = 1e-3;
    g.netlist.nodes = int(nodeIdx.size()) - 1;
    g.netlist.voltageScaleV = vScale;
    g.netlist.currentScaleA = iScale;
    g.elementOfComponent.assign(rs.size(), -1);
    for (std::size_t k = 0; k < rs.size(); ++k) {
        const Resolved& r = rs[k];
        NetElement e;
        e.a = r.a;
        e.b = r.b;
        if (r.src) { e.kind = NetElement::Kind::VoltageSource; e.value = r.param[0]; }
        else {
            e.kind = NetElement::Kind::Device;
            e.model = &g.models_[modelSlot[r.dev->modelId]];
            for (int p = 0; p < 3; ++p) e.param[p] = r.param[p];
        }
        g.elementOfComponent[k] = int(g.netlist.elements.size());
        g.netlist.elements.push_back(e);
    }
    NetElement drive;
    drive.kind = NetElement::Kind::VoltageSource;
    drive.a = nodeIdx.at(bp.input);
    drive.b = 0;
    g.inputElement = int(g.netlist.elements.size());
    g.netlist.elements.push_back(drive);
    g.outputNode = nodeIdx.at(bp.output);

    const double rate = cfg.sampleRate * cfg.oversampling;
    dsp::MnaSolver proto;
    std::string err;
    if (!proto.prepare(g.netlist, 1.0 / rate, {}, err)) {
        d.add(DiagCode::DcFailed, 4, "", "", err);
        return fail(std::move(res), 4);
    }
    cost.dcIterations = proto.lastIterations();
    const double m = proto.unknowns();
    cost.estimatedFlopsPerSample = (2.0 / 3.0) * m * m * m * 3.0 + 2.0 * m * m * 3.0;  // ~3 Newton iterations
    cost.estimatedBytes = std::size_t(m * m + 8 * m) * sizeof(double) + sizeof(PreparedGraph);

    // Check 5: 250 ms synthetic probe (silence, -18 dBFS sine, -6 dBFS transient).
    static const char* const kNames[3] = {"silence", "-18 dBFS sine", "-6 dBFS transient"};
    for (int kind = 0; kind < 3; ++kind) {
        const ProbeResult pr = runProbe(proto, g.inputElement, g.outputNode, rate, cfg.fullScaleVolts, kind);
        cost.probePeakVolts[kind] = pr.peak;
        cost.probeMaxNewton = std::max(cost.probeMaxNewton, pr.maxNewton);
        if (!pr.ok) {
            d.add(DiagCode::ProbeFailed, 5, "", "", std::string(kNames[kind]) + ": " + pr.why);
            return fail(std::move(res), 5);
        }
    }

    // Check 6: bind Mod Slots / macros (stable states map via stableStateMap()).
    auto target = [&](const std::string& comp, const std::string& field) {
        ParamTarget t;
        const int ci = bp.componentIndex(comp);
        if (ci < 0) return t;
        const Resolved& r = rs[std::size_t(ci)];
        t.bound = true;
        t.component = ci;
        t.element = g.elementOfComponent[std::size_t(ci)];
        t.paramIndex = r.dev ? r.dev->paramIndex(field) : -1;
        return t;
    };
    for (const auto& b : bp.modSlots) g.modSlots[std::size_t(b.slot)] = target(b.component, b.field);
    for (const auto& b : bp.macroBindings) g.macroTargets.push_back(target(b.component, b.field));
    cost.validatedForThisConfigurationOnly = true;
    g.cost = cost;
    res.ok = true;
    res.failedCheck = 0;
    res.graph = std::move(graph);
    return res;
}

CompileResult compileText(std::string_view json, const ModelRegistry& reg, const RuntimeConfig& cfg, Generation generation) {
    CompileResult res;
    Blueprint bp;
    if (!parseBlueprint(json, bp, res.diagnostics)) return fail(std::move(res), 1);
    return compile(bp, reg, cfg, generation);
}

std::vector<std::pair<int, int>> stableStateMap(const PreparedGraph& from, const PreparedGraph& to) {
    std::vector<std::pair<int, int>> out;
    for (std::size_t i = 0; i < from.blueprint.components.size(); ++i) {
        const int j = to.blueprint.componentIndex(from.blueprint.components[i].id);
        if (j < 0) continue;
        const BpComponent& a = from.blueprint.components[i];
        const BpComponent& b = to.blueprint.components[std::size_t(j)];
        if (a.modelId != b.modelId || a.pins != b.pins) continue;
        const int ea = from.elementOfComponent[i], eb = to.elementOfComponent[std::size_t(j)];
        const auto& na = from.netlist.elements[std::size_t(ea)];
        const auto& nb = to.netlist.elements[std::size_t(eb)];
        const int ra = na.model ? na.model->revision : 1, rb = nb.model ? nb.model->revision : 1;
        if (ra != rb) continue;
        out.emplace_back(int(i), j);
    }
    return out;
}

CompileResult GraphSlot::compileAndApply(const Blueprint& bp, const ModelRegistry& reg, const RuntimeConfig& cfg) {
    std::lock_guard<std::mutex> serial(applyMu_);
    Generation next;
    { std::lock_guard<std::mutex> l(mu_); next = gen_ + 1; }
    CompileResult r = compile(bp, reg, cfg, next);
    if (r.ok) {
        std::lock_guard<std::mutex> l(mu_);
        active_ = r.graph;
        gen_ = next;
    }
    return r;
}
CompileResult GraphSlot::compileAndApply(std::string_view json, const ModelRegistry& reg, const RuntimeConfig& cfg) {
    Blueprint bp;
    CompileResult r;
    if (!parseBlueprint(json, bp, r.diagnostics)) return fail(std::move(r), 1);
    return compileAndApply(bp, reg, cfg);
}
PreparedHandle GraphSlot::active() const { std::lock_guard<std::mutex> l(mu_); return active_; }
Generation GraphSlot::generation() const { std::lock_guard<std::mutex> l(mu_); return gen_; }

}  // namespace cd::blueprint
