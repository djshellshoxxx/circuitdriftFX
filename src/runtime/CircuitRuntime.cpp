// CircuitDrift runtime: oversampled MNA circuit, thermal clock, latched solver fallback.
// See CircuitRuntime.h for the contracts. Owner: cd_runtime.
#include "runtime/CircuitRuntime.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace cd::runtime {
namespace {

using dsp::DeviceEquation;
using dsp::NetElement;

constexpr std::size_t kMaxThermal = dsp::ThermalNetwork::kMaxNodes;

// One dissipative element mapped to a thermal node (copied scalar data; no manifest pointer).
struct MappedBranch {
    int a = 0, b = 0, node = 0, element = 0;
    bool diode = false;
    double g = 0;                       // resistor conductance
    double is = 0, nvt = 1, cap = 40, expCap = 0;  // diode (same C1 extension as MnaSolver)
};

double branchCurrent(const MappedBranch& m, double v) noexcept {
    if (!m.diode) return m.g * v;
    const double arg = v / m.nvt;
    if (arg > m.cap) return m.is * (m.expCap * (1.0 + arg - m.cap) - 1.0);
    return m.is * (std::exp(arg) - 1.0);
}

NetElement fixtureDevice(const char* id, int a, int b, std::initializer_list<double> p) {
    NetElement e;
    e.model = dsp::findSyntheticFixture(id);
    e.a = a; e.b = b;
    int k = 0;
    for (double v : p) e.param[k++] = v;
    return e;
}

NetElement voltageSource(int a, int b) {
    NetElement e;
    e.kind = NetElement::Kind::VoltageSource;
    e.a = a; e.b = b;
    return e;
}

constexpr double kIs = 1e-14, kN = 1.0, kVt = 0.025852;  // fixture.diode defaults (synthetic)

}  // namespace

CircuitConfig syntheticDiodeClipper() {
    CircuitConfig c;
    c.netlist.nodes = 2;
    c.netlist.elements = {voltageSource(1, 0), fixtureDevice("fixture.resistor", 1, 2, {1000.0}),
                          fixtureDevice("fixture.diode", 2, 0, {kIs, kN, kVt}),
                          fixtureDevice("fixture.diode", 0, 2, {kIs, kN, kVt}),
                          fixtureDevice("fixture.capacitor", 2, 0, {1e-8})};
    c.io = {0, 1.0, 2, 1.0};
    return c;
}

CircuitConfig syntheticRcLowpass(double r, double cap) {
    CircuitConfig c;
    c.netlist.nodes = 2;
    c.netlist.elements = {voltageSource(1, 0), fixtureDevice("fixture.resistor", 1, 2, {r}),
                          fixtureDevice("fixture.capacitor", 2, 0, {cap})};
    c.io = {0, 1.0, 2, 1.0};
    return c;
}

CircuitConfig syntheticDivider(double unitsPerVolt) {
    CircuitConfig c;
    c.netlist.nodes = 2;
    c.netlist.elements = {voltageSource(1, 0), fixtureDevice("fixture.resistor", 1, 2, {1000.0}),
                          fixtureDevice("fixture.resistor", 2, 0, {1000.0})};
    c.io = {0, 1.0, 2, unitsPerVolt};
    return c;
}

struct CircuitRuntime::Core {
    struct Channel {
        dsp::MnaSolver solver;
        dsp::Oversampler os;
        DelayLine pad, dry;
        dsp::ThermalNetwork thermal;
        std::vector<double> hi, low, dcState;
        std::array<double, kMaxThermal> powerSum{}, lastPower{}, energy{};
        std::int64_t consecutive = 0;
    };
    // Owned immutable manifest copies; solver elements point here (stable heap addresses).
    std::vector<std::unique_ptr<const dsp::DeviceManifest>> manifests;
    Channel ch[kMaxChannels];
    std::vector<MappedBranch> mapped;
    std::vector<double> fade;
    std::array<double, kMaxThermal> ambient{};
    LatencyPlan plan;
    SyntheticIo io;
    double fs = 0, ts = 0, fadeStep = 0, fadeW = 0;
    // Exact 1 kHz clock: phase = phaseAcc / fs ticks; each host sample adds 1000 (integer Hz rates).
    int64_t phaseAcc = 0, fsInt = 0;
    int channels = 0, factor = 0, maxBlock = 0, thermalNodes = 0;
    int srcA = 0, srcB = 0;                // input voltage-source element nodes (telemetry)
    bool thermalOn = false, latched = false, resetRequested = false;
    int64_t samples = 0;
    std::int64_t failed = 0, ticks = 0;
    std::uint64_t hash = 0;
    mutable std::vector<double> scratch;   // snapshot staging (sized at prepare)
    std::vector<NetElement> elements;      // prepared netlist (models -> owned manifests): nominal params, bounds
    std::int64_t paramUpdates = 0, paramRefused = 0;
};

namespace {

void countUp(std::int64_t& c) noexcept { if (c < std::numeric_limits<std::int64_t>::max()) ++c; }

// One bounded update on one channel's solver (value already clamped to the manifest range);
// mapped resistor branches follow the new value. Template only so the private Core is deduced.
template <class CoreT>
void pushParameter(CoreT& k, dsp::MnaSolver& s, int element, int p, double v) noexcept {
    if (!s.updateParameter(element, p, v)) { countUp(k.paramRefused); return; }
    countUp(k.paramUpdates);
    const auto& e = k.elements[std::size_t(element)];
    if (e.model->equation == DeviceEquation::Resistor)
        for (auto& m : k.mapped)
            if (m.element == element && !m.diode) m.g = 1.0 / v;
}

}  // namespace

CircuitRuntime::CircuitRuntime() = default;
CircuitRuntime::~CircuitRuntime() = default;

bool CircuitRuntime::prepare(const CircuitConfig& cfg, std::string& err) {
    auto bad = [&](const char* why) { err = why; return false; };
    if (!(cfg.sampleRate >= 44100.0 && cfg.sampleRate <= 192000.0)) return bad("sample rate outside 44.1-192 kHz");
    if (cfg.sampleRate != std::floor(cfg.sampleRate)) return bad("non-integer sample rate unsupported (exact thermal clock)");
    if (cfg.maxBlock < 1 || cfg.maxBlock > dsp::Oversampler::kMaxBlockSize) return bad("maxBlock outside 1..8192");
    if (cfg.channels < 1 || cfg.channels > kMaxChannels) return bad("channels must be 1 or 2");
    const auto& els = cfg.netlist.elements;
    if (cfg.io.sourceElement < 0 || cfg.io.sourceElement >= int(els.size()) ||
        els[std::size_t(cfg.io.sourceElement)].kind != NetElement::Kind::VoltageSource)
        return bad("input mapping must name a voltage-source element");
    if (cfg.io.outputNode < 1 || cfg.io.outputNode > cfg.netlist.nodes) return bad("output node out of range");
    if (!(cfg.io.voltsPerUnit > 0 && std::isfinite(cfg.io.voltsPerUnit) && cfg.io.unitsPerVolt > 0 &&
          std::isfinite(cfg.io.unitsPerVolt)))
        return bad("I/O scales must be finite and positive");

    auto core = std::make_unique<Core>();
    // Deep-copy every referenced manifest; never retain the caller's pointers.
    dsp::Netlist net = cfg.netlist;
    std::vector<const dsp::DeviceManifest*> originals;
    for (auto& e : net.elements) {
        if (e.kind != NetElement::Kind::Device || !e.model) continue;
        std::size_t k = 0;
        while (k < originals.size() && originals[k] != e.model) ++k;
        if (k == originals.size()) {
            originals.push_back(e.model);
            core->manifests.push_back(std::make_unique<const dsp::DeviceManifest>(*e.model));
        }
        e.model = core->manifests[k].get();
    }
    net.elements[std::size_t(cfg.io.sourceElement)].value = 0.0;  // DC point at zero input

    if (!planLatency(cfg.os, cfg.padToFamilyMax, core->plan)) return bad("oversampler design or latency plan failed");
    const double h = 1.0 / (cfg.sampleRate * double(cfg.os.factor));
    dsp::MnaSolver solver;
    if (!solver.prepare(net, h, {cfg.maxNewton, 1e-7}, err)) return false;

    // Thermal contract (SYNTHETIC).
    dsp::ThermalNetwork::Config tcfg = cfg.thermal.network;
    if (tcfg.n == 0) { tcfg.n = 1; tcfg.capacity[0] = 2.0; tcfg.toAmbient[0] = 0.2; }
    core->thermalOn = cfg.thermal.enabled;
    if (core->thermalOn) {
        const auto& map = cfg.thermal.elementToNode;
        if (!map.empty() && map.size() != els.size()) return bad("thermal map size must equal element count");
        for (std::size_t k = 0; k < els.size(); ++k) {
            const auto& e = net.elements[k];
            const bool dissipative = e.kind == NetElement::Kind::Device &&
                                     (e.model->equation == DeviceEquation::Resistor || e.model->equation == DeviceEquation::ShockleyDiode);
            const int node = map.empty() ? (dissipative ? 0 : -1) : map[k];
            if (node < 0) continue;
            if (!dissipative) return bad("thermal map may only reference dissipative (resistor/diode) elements");
            if (std::size_t(node) >= tcfg.n) return bad("thermal node out of range");
            MappedBranch m;
            m.a = e.a; m.b = e.b; m.node = node; m.element = int(k);
            if (e.model->equation == DeviceEquation::Resistor) {
                m.g = 1.0 / e.param[0];
            } else {
                m.diode = true; m.is = e.param[0]; m.nvt = e.param[1] * e.param[2];
                m.cap = e.model->maxExpArg; m.expCap = std::exp(m.cap);
            }
            core->mapped.push_back(m);
        }
        if (!(cfg.thermal.ambientK > 0 && std::isfinite(cfg.thermal.ambientK))) return bad("ambient must be positive kelvin");
    }

    core->fs = cfg.sampleRate; core->ts = 1.0 / cfg.sampleRate;
    core->fsInt = int64_t(cfg.sampleRate);
    core->fadeStep = 1.0 / std::max(1.0, std::round(kFallbackFadeSec * cfg.sampleRate));
    core->channels = cfg.channels; core->factor = cfg.os.factor; core->maxBlock = cfg.maxBlock;
    core->io = cfg.io;
    core->srcA = els[std::size_t(cfg.io.sourceElement)].a; core->srcB = els[std::size_t(cfg.io.sourceElement)].b;
    core->thermalNodes = int(tcfg.n);
    for (std::size_t i = 0; i < kMaxThermal; ++i) core->ambient[i] = cfg.thermal.ambientK;
    core->fade.assign(std::size_t(cfg.maxBlock), 0.0);
    for (int c = 0; c < cfg.channels; ++c) {
        auto& ch = core->ch[c];
        ch.solver = solver;  // copies point at core-owned manifests
        ch.dcState.assign(std::size_t(solver.stateSize()), 0.0);
        ch.solver.getState(ch.dcState.data());
        if (!ch.os.prepare(cfg.os, cfg.maxBlock)) return bad("oversampler prepare failed");
        ch.pad.prepare(core->plan.padHigh);
        ch.dry.prepare(core->plan.declared);
        ch.hi.assign(std::size_t(cfg.maxBlock) * std::size_t(cfg.os.factor), 0.0);
        ch.low.assign(std::size_t(cfg.maxBlock), 0.0);
        if (core->thermalOn && !ch.thermal.prepare(tcfg, 1.0 / kThermalRateHz, cfg.thermal.ambientK))
            return bad("thermal network rejected (C>0, G>=0, ambient path per island)");
    }
    std::size_t scratch = 0;
    for (int c = 0; c < cfg.channels; ++c)
        scratch = std::max({scratch, std::size_t(core->ch[c].solver.stateSize()), std::size_t(core->ch[c].os.stateSize())});
    core->scratch.assign(scratch + 1, 0.0);
    Fnv64 fnv;
    fnv.str("cd.runtime.CircuitRuntime/1");
    fnv.pod(cfg.channels); fnv.pod(cfg.os.factor); fnv.pod(cfg.os.family); fnv.pod(cfg.os.passFraction); fnv.pod(cfg.os.stopbandDb);
    fnv.pod(cfg.maxNewton); fnv.pod(cfg.padToFamilyMax); fnv.pod(core->plan.declared); fnv.pod(core->plan.padHigh);
    fnv.pod(net.nodes); fnv.pod(net.voltageScaleV); fnv.pod(net.currentScaleA);
    for (const auto& e : net.elements) {
        fnv.pod(e.kind); fnv.pod(e.a); fnv.pod(e.b); fnv.pod(e.param); fnv.pod(e.value);
        if (e.model) { fnv.str(e.model->modelId.c_str()); fnv.pod(e.model->revision); fnv.pod(e.model->equation); }
    }
    fnv.pod(cfg.io.sourceElement); fnv.pod(cfg.io.voltsPerUnit); fnv.pod(cfg.io.outputNode); fnv.pod(cfg.io.unitsPerVolt);
    fnv.pod(core->thermalOn); fnv.pod(tcfg.n); fnv.pod(cfg.thermal.ambientK);
    for (std::size_t i = 0; i < tcfg.n; ++i) {
        fnv.pod(tcfg.capacity[i]); fnv.pod(tcfg.toAmbient[i]);
        for (std::size_t j = 0; j < tcfg.n; ++j) fnv.pod(tcfg.coupling[i][j]);
    }
    for (const auto& m : core->mapped) { fnv.pod(m.a); fnv.pod(m.b); fnv.pod(m.node); }
    core->hash = fnv.h;
    core->elements = net.elements;   // model pointers -> core-owned manifests
    core_ = std::move(core);  // install only after every step succeeded
    err.clear();
    return true;
}

void CircuitRuntime::process(const double* const* in, double* const* wet, double* const* dry, int n,
                             const double* drive, const ParamTrack* tracks, int trackCount) noexcept {
    Core* k = core_.get();
    if (!k || n <= 0 || n > k->maxBlock || !in || !wet || !dry) return;
    for (int c = 0; c < k->channels; ++c)
        if (!in[c] || !wet[c] || !dry[c]) return;
    const int F = k->factor;
    // Validate the parameter tracks once per call (bounded: <= kMaxParamTracks).
    struct Track { int element, p; double lo, hi; const double* v; };
    Track tr[kMaxParamTracks];
    int nt = 0;
    if (tracks)
        for (int t = 0; t < trackCount && t < kMaxParamTracks; ++t) {
            const ParamTrack& x = tracks[t];
            double nom = 0, lo = 0, hi = 0;
            if (!x.values || !parameterRange(x.element, x.k, nom, lo, hi)) { countUp(k->paramRefused); continue; }
            tr[nt++] = {x.element, x.k, lo, hi, x.values};
        }
    for (int c = 0; c < k->channels; ++c) {
        auto& ch = k->ch[c];
        if (drive) {  // pre-stage drive on the circuit input only (ch.low is free until down())
            for (int i = 0; i < n; ++i) ch.low[std::size_t(i)] = in[c][i] * drive[i];
            ch.os.up(ch.low.data(), ch.hi.data(), n);
        } else {
            ch.os.up(in[c], ch.hi.data(), n);
        }
    }

    std::array<double, kMaxThermal> tickPower{};
    for (int i = 0; i < n; ++i) {
        for (int c = 0; c < k->channels; ++c) {
            auto& ch = k->ch[c];
            double* hs = ch.hi.data() + std::size_t(i) * std::size_t(F);
            std::fill(ch.powerSum.begin(), ch.powerSum.begin() + k->thermalNodes, 0.0);
            for (int s = 0; s < F; ++s) {
                for (int t = 0; t < nt; ++t) {
                    double v = tr[t].v[std::size_t(i) * std::size_t(F) + std::size_t(s)];
                    if (!std::isfinite(v)) { countUp(k->paramRefused); continue; }
                    v = std::clamp(v, tr[t].lo, tr[t].hi);   // manifest range first: updateParameter never sees out-of-range
                    if (ch.solver.parameter(tr[t].element, tr[t].p) != v) pushParameter(*k, ch.solver, tr[t].element, tr[t].p, v);
                }
                ch.solver.setSource(k->io.sourceElement, ch.pad.process(hs[s]) * k->io.voltsPerUnit);
                if (ch.solver.step()) {
                    ch.consecutive = 0;
                } else {
                    if (k->failed < std::numeric_limits<std::int64_t>::max()) ++k->failed;
                    if (ch.consecutive < std::numeric_limits<std::int64_t>::max()) ++ch.consecutive;
                    if (ch.consecutive >= kFallbackAfterConsecutive && !k->latched) { k->latched = true; k->resetRequested = true; }
                }
                hs[s] = ch.solver.voltage(k->io.outputNode) * k->io.unitsPerVolt;
                if (k->thermalOn)
                    for (const auto& m : k->mapped) {
                        const double v = ch.solver.voltage(m.a) - ch.solver.voltage(m.b);
                        const double p = v * branchCurrent(m, v);
                        if (isFiniteSample(p)) ch.powerSum[std::size_t(m.node)] += p;
                    }
            }
            for (int q = 0; q < k->thermalNodes; ++q) ch.lastPower[std::size_t(q)] = ch.powerSum[std::size_t(q)] / double(F);
        }
        // 1 kHz thermal clock (exact fractional phase; boundary sample energy split exactly).
        const int64_t next = k->phaseAcc + int64_t(kThermalRateHz);
        if (next >= k->fsInt) {
            const double a = double(k->fsInt - k->phaseAcc) / kThermalRateHz;  // part of this sample before the tick
            for (int c = 0; c < k->channels && k->thermalOn; ++c) {
                auto& ch = k->ch[c];
                for (int q = 0; q < k->thermalNodes; ++q) {
                    const std::size_t u = std::size_t(q);
                    tickPower[u] = (ch.energy[u] + ch.lastPower[u] * a * k->ts) * kThermalRateHz;
                    ch.energy[u] = ch.lastPower[u] * (1.0 - a) * k->ts;
                }
                ch.thermal.step(tickPower.data());
            }
            k->phaseAcc = next - k->fsInt;
            ++k->ticks;
        } else {
            for (int c = 0; c < k->channels && k->thermalOn; ++c)
                for (int q = 0; q < k->thermalNodes; ++q)
                    k->ch[c].energy[std::size_t(q)] += k->ch[c].lastPower[std::size_t(q)] * k->ts;
            k->phaseAcc = next;
        }
        if (k->latched) k->fadeW = std::min(1.0, k->fadeW + k->fadeStep);
        k->fade[std::size_t(i)] = k->fadeW;
        ++k->samples;
    }

    for (int c = 0; c < k->channels; ++c) {
        auto& ch = k->ch[c];
        ch.os.down(ch.hi.data(), ch.low.data(), n);
        for (int i = 0; i < n; ++i) {
            const double d = ch.dry.process(in[c][i]);
            const double f = k->fade[std::size_t(i)];
            const double w = ch.low[std::size_t(i)];
            dry[c][i] = d;
            wet[c][i] = f == 0.0 ? w : (f == 1.0 ? d : (1.0 - f) * w + f * d);
        }
    }
}

void CircuitRuntime::reset() noexcept {
    Core* k = core_.get();
    if (!k) return;
    for (int c = 0; c < k->channels; ++c) {
        auto& ch = k->ch[c];
        // Prepared (nominal) parameters first, so the DC state is consistent with them.
        for (std::size_t e = 0; e < k->elements.size(); ++e) {
            const auto& el = k->elements[e];
            if (el.kind != NetElement::Kind::Device || !el.model) continue;
            for (std::size_t q = 0; q < el.model->params.size() && q < 3; ++q)
                if (ch.solver.parameter(int(e), int(q)) != el.param[q]) pushParameter(*k, ch.solver, int(e), int(q), el.param[q]);
        }
        ch.solver.setState(ch.dcState.data());
        ch.os.reset(); ch.pad.reset(); ch.dry.reset();
        if (k->thermalOn) ch.thermal.setState(k->ambient.data());
        ch.powerSum.fill(0.0); ch.lastPower.fill(0.0); ch.energy.fill(0.0);
        ch.consecutive = 0;
    }
    k->phaseAcc = 0; k->ticks = 0; k->fadeW = 0;
    k->latched = false; k->resetRequested = false;  // failure count kept as a diagnostic
}

void CircuitRuntime::saveState(StateWriter& w) const noexcept {
    const Core* k = core_.get();
    if (!k) return;
    const std::int64_t g[6] = {k->phaseAcc, k->ticks, k->samples, k->failed, k->latched ? 1 : 0, k->resetRequested ? 1 : 0};
    w.pod(g);
    w.pod(k->fadeW);
    const std::size_t T = std::size_t(k->thermalNodes);
    for (int c = 0; c < k->channels; ++c) {
        const auto& ch = k->ch[c];
        ch.solver.getState(k->scratch.data());
        w.doubles(k->scratch.data(), std::size_t(ch.solver.stateSize()));
        ch.os.getState(k->scratch.data());
        w.doubles(k->scratch.data(), std::size_t(ch.os.stateSize()));
        ch.pad.save(w);
        ch.dry.save(w);
        for (std::size_t q = 0; q < T; ++q) {
            const double t[4] = {k->thermalOn ? ch.thermal.temperature(q) : 0.0, ch.powerSum[q], ch.lastPower[q], ch.energy[q]};
            w.pod(t);
        }
        const std::int64_t cons = ch.consecutive;
        w.pod(cons);
    }
}

bool CircuitRuntime::loadState(StateReader& r, bool apply) noexcept {
    Core* k = core_.get();
    if (!k) return false;
    std::int64_t g[6] = {};
    double fadeW = 0;
    if (!r.pod(g) || !r.pod(fadeW)) return false;
    if (g[0] < 0 || g[0] >= k->fsInt || g[1] < 0 || g[2] < 0 || g[3] < 0 || !(fadeW >= 0 && fadeW <= 1)) return false;
    if (apply) {
        k->phaseAcc = g[0]; k->ticks = std::int64_t(g[1]); k->samples = g[2]; k->failed = std::int64_t(g[3]);
        k->latched = g[4] != 0; k->resetRequested = g[5] != 0; k->fadeW = fadeW;
    }
    const std::size_t T = std::size_t(k->thermalNodes);
    for (int c = 0; c < k->channels; ++c) {
        auto& ch = k->ch[c];
        if (!r.doubles(k->scratch.data(), std::size_t(ch.solver.stateSize()))) return false;
        if (apply) ch.solver.setState(k->scratch.data());
        if (!r.doubles(k->scratch.data(), std::size_t(ch.os.stateSize()))) return false;
        if (!(apply ? ch.os.setState(k->scratch.data()) : ch.os.stateValid(k->scratch.data()))) return false;
        if (!ch.pad.load(r, apply) || !ch.dry.load(r, apply)) return false;
        std::array<double, kMaxThermal> temps{};
        for (std::size_t q = 0; q < T; ++q) {
            double t[4] = {};
            if (!r.pod(t)) return false;
            if (k->thermalOn && !(t[0] > 0 && std::isfinite(t[0]))) return false;
            temps[q] = t[0];
            if (apply) { ch.powerSum[q] = t[1]; ch.lastPower[q] = t[2]; ch.energy[q] = t[3]; }
        }
        if (apply && k->thermalOn) ch.thermal.setState(temps.data());
        std::int64_t cons = 0;
        if (!r.pod(cons) || cons < 0) return false;
        if (apply) ch.consecutive = std::int64_t(cons);
    }
    return true;
}

std::size_t CircuitRuntime::stateBytes() const noexcept {
    StateWriter w(nullptr, 0);
    saveState(w);
    return w.size();
}

std::uint64_t CircuitRuntime::modelHash() const noexcept { return core_ ? core_->hash : 0; }

int CircuitRuntime::latencySamples() const noexcept { return core_ ? core_->plan.declared : 0; }
const LatencyPlan& CircuitRuntime::latencyPlan() const noexcept {
    static const LatencyPlan kNone{};
    return core_ ? core_->plan : kNone;
}
int CircuitRuntime::channels() const noexcept { return core_ ? core_->channels : 0; }
int CircuitRuntime::factor() const noexcept { return core_ ? core_->factor : 0; }
int CircuitRuntime::maxBlock() const noexcept { return core_ ? core_->maxBlock : 0; }
double CircuitRuntime::sampleRate() const noexcept { return core_ ? core_->fs : 0.0; }
bool CircuitRuntime::parameterRange(int element, int k, double& nominal, double& min, double& max) const noexcept {
    if (!core_ || element < 0 || element >= int(core_->elements.size())) return false;
    const auto& e = core_->elements[std::size_t(element)];
    if (e.kind != NetElement::Kind::Device || !e.model || k < 0 || k >= int(e.model->params.size()) || k >= 3) return false;
    nominal = e.param[k];
    min = e.model->params[std::size_t(k)].min;
    max = e.model->params[std::size_t(k)].max;
    return true;
}

double CircuitRuntime::parameter(int c, int element, int k) const noexcept {
    if (!core_ || c < 0 || c >= core_->channels) return std::numeric_limits<double>::quiet_NaN();
    return core_->ch[c].solver.parameter(element, k);
}

std::int64_t CircuitRuntime::parameterUpdates() const noexcept { return core_ ? core_->paramUpdates : 0; }
std::int64_t CircuitRuntime::parameterRefusals() const noexcept { return core_ ? core_->paramRefused : 0; }

int CircuitRuntime::ownedManifestCount() const noexcept { return core_ ? int(core_->manifests.size()) : 0; }

CircuitRuntime::Diagnostics CircuitRuntime::diagnostics() const noexcept {
    Diagnostics d;
    if (!core_) return d;
    const Core& k = *core_;
    d.samples = k.samples; d.failedSubsteps = k.failed;
    for (int c = 0; c < k.channels; ++c) d.consecutiveFailures = std::max(d.consecutiveFailures, k.ch[c].consecutive);
    d.fallbackLatched = k.latched; d.resetRequested = k.resetRequested; d.fallbackWeight = k.fadeW;
    d.thermalTicks = k.ticks; d.thermalPhase = double(k.phaseAcc) / double(k.fsInt);
    return d;
}

double CircuitRuntime::temperatureK(int c, int node) const noexcept {
    if (!core_ || !core_->thermalOn || c < 0 || c >= core_->channels || node < 0 || node >= core_->thermalNodes) return 0.0;
    return core_->ch[c].thermal.temperature(std::size_t(node));
}

double CircuitRuntime::lastPowerW(int c, int node) const noexcept {
    if (!core_ || c < 0 || c >= core_->channels || node < 0 || node >= core_->thermalNodes) return 0.0;
    return core_->ch[c].lastPower[std::size_t(node)];
}

double CircuitRuntime::voltage(int c, int node) const noexcept {
    if (!core_ || c < 0 || c >= core_->channels) return 0.0;
    return core_->ch[c].solver.voltage(node);
}

int CircuitRuntime::telemetryBranches() const noexcept { return core_ && core_->thermalOn ? int(core_->mapped.size()) : 0; }

bool CircuitRuntime::branchTelemetry(int c, int k, BranchTelemetry& out) const noexcept {
    if (!core_ || !core_->thermalOn || c < 0 || c >= core_->channels || k < 0 || k >= int(core_->mapped.size())) return false;
    const MappedBranch& m = core_->mapped[std::size_t(k)];
    const auto& s = core_->ch[c].solver;
    const double v = s.voltage(m.a) - s.voltage(m.b);
    const double i = branchCurrent(m, v);
    out.element = m.element;
    out.thermalNode = m.node;
    out.voltageV = v;
    out.currentA = std::isfinite(i) ? i : 0.0;
    out.powerW = std::isfinite(v * i) ? v * i : 0.0;
    out.temperatureK = core_->ch[c].thermal.temperature(std::size_t(m.node));
    return true;
}

double CircuitRuntime::inputVolts(int c) const noexcept {
    if (!core_ || c < 0 || c >= core_->channels) return 0.0;
    const auto& s = core_->ch[c].solver;
    return s.voltage(core_->srcA) - s.voltage(core_->srcB);
}

double CircuitRuntime::outputVolts(int c) const noexcept {
    if (!core_ || c < 0 || c >= core_->channels) return 0.0;
    return core_->ch[c].solver.voltage(core_->io.outputNode);
}

int CircuitRuntime::lastIterations(int c) const noexcept {
    if (!core_ || c < 0 || c >= core_->channels) return 0;
    return core_->ch[c].solver.lastIterations();
}

double CircuitRuntime::lastResidual(int c) const noexcept {
    if (!core_ || c < 0 || c >= core_->channels) return 0.0;
    return core_->ch[c].solver.lastScaledResidual();
}

}  // namespace cd::runtime
