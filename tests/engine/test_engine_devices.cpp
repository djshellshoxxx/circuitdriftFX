// Engine device catalogue tests (P03): synthetic circuit manifests, analytic Jacobians of every
// new MNA family against central differences, electrolytic/inductor state exactness and runtime
// parameter/step-size changes. All models are SYNTHETIC FIXTURES (G01), not calibrated hardware.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "core/Json.h"
#include "dsp/DeviceManifest.h"
#include "dsp/MnaSolver.h"
#include "engine/Circuit.h"
#include "engine/EngineTypes.h"

namespace {
int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL %s:%d %s | ", __FILE__, __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
using namespace cd;
using K = dsp::NetElement::Kind;

dsp::NetElement dev(const char* id, int a, int b, std::initializer_list<double> p, int c = 0, int d = 0) {
    dsp::NetElement e;
    e.model = dsp::findSyntheticDevice(id);
    e.a = a; e.b = b; e.c = c; e.d = d;
    int k = 0;
    for (double v : p) e.param[k++] = v;
    return e;
}
dsp::NetElement src(K k, int a, int b, double v) { dsp::NetElement e; e.kind = k; e.a = a; e.b = b; e.value = v; return e; }
std::vector<double> defaults(const char* id) {
    std::vector<double> v;
    for (const auto& p : dsp::findSyntheticDevice(id)->params) v.push_back(p.def);
    return v;
}
dsp::NetElement devDefault(const char* id, int a, int b, int c = 0, int d = 0) {
    dsp::NetElement e;
    e.model = dsp::findSyntheticDevice(id);
    e.a = a; e.b = b; e.c = c; e.d = d;
    const auto v = defaults(id);
    std::copy(v.begin(), v.end(), e.param);
    return e;
}

void manifests() {
    const auto& cat = dsp::syntheticCircuitCatalogue();
    CHECK(int(cat.size()) == dsp::kSyntheticCircuitCount, "circuit catalogue %zu", cat.size());
    for (const auto& m : cat) {
        CHECK(m.provenance == dsp::ProvenanceKind::SyntheticFixture, "%s synthetic", m.modelId.c_str());
        CHECK(m.provenanceNote.find("synthetic-fixture, not calibrated hardware (G01)") != std::string::npos, "%s note", m.modelId.c_str());
        CHECK(dsp::findSyntheticDevice(m.modelId) == &m, "lookup %s", m.modelId.c_str());
    }
    CHECK(dsp::syntheticFixtureCatalogue().size() == 3, "fixture catalogue unchanged");
    CHECK(dsp::findSyntheticFixture("synthetic.triode-12ax7") == nullptr, "fixture lookup scope unchanged");
    CHECK(dsp::findSyntheticDevice("fixture.diode") != nullptr, "device lookup includes fixtures");
    // Mutations of a circuit manifest are rejected (wrong pin count, missing domain).
    core::JsonValue j; std::string e; dsp::DeviceManifest m;
    std::string t = dsp::syntheticCircuitJson(3);
    const auto at = t.find("\"pins\":[\"grid\",\"plate\",\"cathode\"]");
    CHECK(at != std::string::npos, "triode anchor");
    t.replace(at, std::string("\"pins\":[\"grid\",\"plate\",\"cathode\"]").size(), "\"pins\":[\"grid\",\"plate\"]");
    CHECK(!(core::parseJson(t, j, e) && dsp::parseDeviceManifest(j, m, e)), "2-pin triode accepted");
    std::string n = dsp::syntheticCircuitJson(5);
    const auto d = n.find(",\"domain\":{\"maxExpArg\":29}");
    CHECK(d != std::string::npos, "npn anchor");
    n.erase(d, std::string(",\"domain\":{\"maxExpArg\":29}").size());
    CHECK(!(core::parseJson(n, j, e) && dsp::parseDeviceManifest(j, m, e)), "NPN without domain accepted");
    // Component table: stable unique keys and synthetic provenance everywhere.
    const auto* tab = engine::componentTable();
    for (int i = 0; i < engine::kComponentCount; ++i) {
        CHECK(int(tab[i].id) == i, "table order %d", i);
        CHECK(std::string(tab[i].provenance).find("synthetic-fixture, not calibrated hardware (G01)") == 0, "%s provenance", tab[i].key);
        CHECK(engine::findComponent(tab[i].key) == &tab[i], "find %s", tab[i].key);
    }
    for (const char* key : {"C_in", "V1", "R_load", "R_k", "C_k", "C_out", "T_out", "C_res", "D_rect"})
        CHECK(engine::findComponent(key) != nullptr, "component %s", key);
}

// Analytic Jacobian vs central differences at several operating points for a netlist.
void jacobianCheck(const char* tag, const dsp::Netlist& n, const std::vector<std::vector<double>>& points, double h = 1e-5) {
    dsp::MnaSolver s;
    std::string e;
    if (!s.prepare(n, h, {8, 1e-7}, e)) { CHECK(false, "%s prepare: %s", tag, e.c_str()); return; }
    const int m = s.unknowns();
    for (const auto& x0 : points) {
        std::vector<double> x = x0, F(m), J(std::size_t(m) * m), Fp(m), Fm(m);
        x.resize(std::size_t(m), 0.0);
        s.evaluate(x.data(), F.data(), J.data(), false);
        double worst = 0;
        for (int c = 0; c < m; ++c) {
            const double hs = 1e-6 * (1 + std::fabs(x[c]));
            auto xp = x, xm = x; xp[c] += hs; xm[c] -= hs;
            s.evaluate(xp.data(), Fp.data(), nullptr, false);
            s.evaluate(xm.data(), Fm.data(), nullptr, false);
            for (int r = 0; r < m; ++r) {
                double rowMax = 0;
                for (int q = 0; q < m; ++q) rowMax = std::max(rowMax, std::fabs(J[std::size_t(r) * m + q]));
                const double fd = (Fp[r] - Fm[r]) / (2 * hs), an = J[std::size_t(r) * m + c];
                worst = std::max(worst, std::fabs(fd - an) / (rowMax + 1e-9));
            }
        }
        CHECK(worst <= 1e-5, "%s Jacobian row-relative error %g", tag, worst);
    }
}

void jacobians() {
    // Triode: V source on grid via 1k, plate via 100k to 250 V source, cathode R.
    {
        dsp::Netlist n; n.nodes = 5; n.voltageScaleV = 100;
        n.elements = {src(K::VoltageSource, 1, 0, -1.0), dev("fixture.resistor", 1, 2, {1e3}), src(K::VoltageSource, 4, 0, 250),
                      dev("fixture.resistor", 4, 3, {1e5}), devDefault("synthetic.triode-12ax7", 2, 3, 5),
                      dev("fixture.resistor", 5, 0, {1500})};
        jacobianCheck("triode", n, {{-1, -1.2, 180, 250, 1.3}, {0.5, 0.3, 50, 250, 1.0}, {-3, -3, 260, 250, 0.1}, {0.2, 0.2, 5, 250, 0.1}});
    }
    // Pentode: screen node driven through 470k.
    {
        dsp::Netlist n; n.nodes = 6; n.voltageScaleV = 100;
        n.elements = {src(K::VoltageSource, 1, 0, -1.0), dev("fixture.resistor", 1, 2, {1e3}), src(K::VoltageSource, 4, 0, 250),
                      dev("fixture.resistor", 4, 3, {1e5}), devDefault("synthetic.pentode-ef86", 2, 3, 5, 6),
                      dev("fixture.resistor", 5, 0, {2200}), dev("fixture.resistor", 4, 6, {4.7e5})};
        jacobianCheck("pentode", n, {{-1, -1.5, 150, 250, 3, 140}, {0.2, 0.1, 20, 250, 2, 100}, {-2, -4, 220, 250, 1, 200}});
    }
    // NPN with divider bias and emitter resistor, including the capped exponent region.
    {
        dsp::Netlist n; n.nodes = 4;
        n.elements = {src(K::VoltageSource, 1, 0, 30), dev("fixture.resistor", 1, 2, {82e3}), dev("fixture.resistor", 2, 0, {15e3}),
                      dev("fixture.resistor", 1, 3, {1e4}), devDefault("synthetic.npn-small-signal", 2, 3, 4),
                      dev("fixture.resistor", 4, 0, {2700})};
        jacobianCheck("npn", n, {{30, 4.3, 15, 3.64}, {30, 0.5, 10, 0.0}, {30, 5, 3.9, 4.0}, {30, 3, 1, 1.0}, {30, 2.0, 15, 0.5}});
    }
    // N-JFET (triode, saturation, reverse Vds, gate conduction).
    {
        dsp::Netlist n; n.nodes = 4;
        n.elements = {src(K::VoltageSource, 1, 0, 30), dev("fixture.resistor", 2, 0, {1e6}), dev("fixture.resistor", 1, 3, {12e3}),
                      devDefault("synthetic.njfet-small-signal", 2, 3, 4), dev("fixture.resistor", 4, 0, {1000})};
        jacobianCheck("njfet", n, {{30, 0, 19, 0.8}, {30, 0.5, 0.3, 0.2}, {30, 0, -0.5, 0.2}, {30, 0.9, 5, 0.1}, {30, -3, 10, 0.5}});
    }
    // Electrolytic + saturating inductor (transient stamps include history/flux terms).
    {
        dsp::Netlist n; n.nodes = 2; n.voltageScaleV = 10;
        n.elements = {src(K::VoltageSource, 1, 0, 0.0), devDefault("synthetic.electrolytic", 1, 2), devDefault("synthetic.output-transformer", 2, 0),
                      dev("fixture.resistor", 2, 0, {1e5})};
        jacobianCheck("electrolytic+inductor", n, {{1.0, 0.5, 0.0}, {50.0, 30.0, 1e-3}, {-100, -80, 0}}, 1.0 / 192000);
    }
}

void stateExactness() {
    std::string e;
    // Electrolytic RC step: ESR + C || leakage driven by 1 V through R; exact BE recurrence.
    {
        const double R = 1e4, C = 1e-6, esr = 10, rl = 1e7, h = 1.0 / 192000;
        dsp::Netlist n; n.nodes = 2;
        n.elements = {src(K::VoltageSource, 1, 0, 0.0), dev("fixture.resistor", 1, 2, {R}), dev("synthetic.electrolytic", 2, 0, {C, esr, rl})};
        dsp::MnaSolver s; CHECK(s.prepare(n, h, {8, 1e-7}, e), "elec prepare %s", e.c_str());
        s.setSource(0, 1.0);
        double vc = 0, worst = 0;
        for (int i = 0; i < 2000; ++i) {
            CHECK(s.step(), "elec step %d", i);
            // node: (1 - v)/R = (v - vc')/esr ; C (vc' - vc)/h = (v - vc')/esr - vc'/rl
            const double a = C / h, ge = 1 / esr, gl = 1 / rl;
            // eliminate: vc' = (a vc + ge v)/(a + ge + gl); (1-v)/R = ge (v - vc')
            const double D = a + ge + gl;
            const double v = (1.0 / R + ge * a * vc / D) / (1.0 / R + ge - ge * ge / D);
            vc = (a * vc + ge * v) / D;
            worst = std::max(worst, std::fabs(s.voltage(2) - v));
        }
        CHECK(worst <= 1e-11, "electrolytic vs BE recurrence %g", worst);
        CHECK(std::fabs(s.elementState(2) - vc) <= 1e-11, "internal capacitor voltage %g vs %g", s.elementState(2), vc);
        // Runtime C change preserves the internal capacitor voltage (no step).
        const double before = s.elementState(2);
        CHECK(s.setElementParam(2, 0, 2e-6) && s.elementState(2) == before, "electrolytic value change keeps state");
        CHECK(!s.setElementParam(2, 0, NAN) && s.elementParam(2, 0) == 2e-6, "non-finite param ignored");
        CHECK(s.setElementParam(2, 1, 1e9) && s.elementParam(2, 1) == 1e5, "param clamped to manifest bound");
    }
    // Inductor: flux = integral of terminal voltage; DC init inverts the magnetizing curve.
    {
        const double h = 1.0 / 192000;
        dsp::Netlist n; n.nodes = 2;
        n.elements = {src(K::VoltageSource, 1, 0, 0.0), dev("fixture.resistor", 1, 2, {1e3}),
                      dev("synthetic.output-transformer", 2, 0, {400, 0.4, 4, 200})};
        dsp::MnaSolver s; CHECK(s.prepare(n, h, {8, 1e-7}, e), "ind prepare %s", e.c_str());
        double lam = 0, worst = 0;
        for (int i = 0; i < 4000; ++i) {
            s.setSource(0, 100.0 * std::sin(2 * M_PI * 50 * i * h));
            CHECK(s.step(), "ind step %d", i);
            lam += h * s.voltage(2);
            worst = std::max(worst, std::fabs(s.elementState(2) - lam));
            const double u = std::fabs(lam / 0.4);
            const double iexp = lam / 400 * (1 + u * u * u * u);
            if (i % 97 == 0) CHECK(std::fabs(s.pinCurrent(2, 0) - iexp) <= 1e-12 + 1e-9 * std::fabs(iexp), "i(lambda) %g vs %g", s.pinCurrent(2, 0), iexp);
        }
        CHECK(worst <= 1e-12, "flux integral %g", worst);
        dsp::Netlist dc = n; dc.elements[0].value = 0.01;  // DC current 10 uA through the winding
        dsp::MnaSolver d; CHECK(d.prepare(dc, h, {8, 1e-7}, e), "ind dc %s", e.c_str());
        const double ii = (0.01 - d.voltage(2)) / 1e3, l = d.elementState(2), u = std::fabs(l / 0.4);
        CHECK(std::fabs(l / 400 * (1 + u * u * u * u) - ii) <= 1e-12, "DC flux inverse %g", l);
        // Step-size change keeps flux and voltages (quality changes).
        std::vector<double> before(std::size_t(s.stateSize())), after(before.size());
        s.getState(before.data());
        CHECK(s.setStepSize(h / 4) && s.stepSize() == h / 4, "set step size");
        s.getState(after.data());
        CHECK(before == after, "step size change keeps state");
        CHECK(!s.setStepSize(0) && !s.setStepSize(NAN), "invalid step size rejected");
    }
    // Rejections for multi-terminal devices.
    {
        dsp::MnaSolver s;
        dsp::Netlist n; n.nodes = 3;
        n.elements = {src(K::VoltageSource, 1, 0, 1.0), dev("fixture.resistor", 1, 2, {1e3}), devDefault("synthetic.triode-12ax7", 2, 2, 3),
                      dev("fixture.resistor", 3, 0, {1e3})};
        CHECK(!s.prepare(n, 1e-5, {}, e), "duplicate triode pins accepted");
        n.elements[2] = devDefault("synthetic.triode-12ax7", 2, 3, 0);
        n.elements[3] = dev("fixture.resistor", 3, 0, {1e3});
        CHECK(s.prepare(n, 1e-5, {}, e), "grounded-cathode triode: %s", e.c_str());
        n.elements[2] = devDefault("synthetic.triode-12ax7", 2, 3, 4);
        CHECK(!s.prepare(n, 1e-5, {}, e), "out-of-range pin accepted");
    }
}

// Factory circuit DC points stay in the documented synthetic ranges for every family.
void factoryCircuits() {
    for (int f = 0; f < engine::kFamilyCount; ++f) {
        const auto fam = engine::Family(f);
        const auto& d = engine::familyDesign(fam);
        engine::CircuitPath p; std::string e;
        CHECK(p.build(fam, engine::nominalValues(fam, 1), 1.0 / 192000, 8, d.railDesignV * 0.97, e), "family %d build %s", f, e.c_str());
        const double vout = p.nodeVolts(engine::kNodeOut), vb = p.localBplus(), i = p.supplyCurrent();
        CHECK(vout > 0.2 * vb && vout < 0.9 * vb, "family %d output node %g of B+ %g", f, vout, vb);
        CHECK(i > 0.2e-3 && i < 3e-3, "family %d stage current %g A", f, i);
        double pw[engine::kComponentCount] = {};
        p.accumulatePower(pw);
        for (double x : pw) CHECK(std::isfinite(x) && x >= -1e-9, "family %d power %g", f, x);
        CHECK(pw[int(engine::ComponentId::V1)] > 0, "family %d device dissipation", f);
    }
}
}  // namespace

int main() {
    manifests();
    jacobians();
    stateExactness();
    factoryCircuits();
    std::printf(g_fail ? "ENGINE DEVICES FAILED (%d)\n" : "ENGINE DEVICES PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
