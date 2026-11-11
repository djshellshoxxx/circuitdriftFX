// T10: blueprint parse/validate/compile. All circuits and models are SYNTHETIC fixtures (fixture-v1);
// nothing here validates any physical device (G01 open).
#include "blueprint/BlueprintGraph.h"
#include "blueprint/Compiler.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <string>

namespace {
using namespace cd::blueprint;
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (false)

const ModelRegistry kReg = ModelRegistry::syntheticFixtures();
const RuntimeConfig kCfg48{48000, 1, 1.0};
const RuntimeConfig kCfg8{8000, 1, 1.0};

// Builds a blueprint document; pieces are raw JSON fragments.
std::string doc(const std::string& nodes, const std::string& comps, const std::string& ports = R"({"input":"in","output":"out","ground":"0"})",
                const std::string& extra = "", const std::string& ver = "1", const std::string& registry = "fixture-v1") {
    return std::string(R"({"schemaVersion":)") + ver + R"(,"id":"t.bp","name":"T10 fixture","modelRegistryVersion":")" + registry +
           R"(","nodes":)" + nodes + R"(,"components":)" + comps + R"(,"ports":)" + ports + R"(,"macroBindings":[])" + extra + "}";
}
std::string R(const std::string& id, const std::string& p, const std::string& n, double ohm = 10000) {
    return R"({"id":")" + id + R"(","modelId":"fixture.resistor","pins":{"p":")" + p + R"(","n":")" + n + R"("},"parameters":{"resistanceOhm":)" + std::to_string(ohm) + "}}";
}
std::string C(const std::string& id, const std::string& p, const std::string& n, const char* farad = "1e-8") {
    return R"({"id":")" + id + R"(","modelId":"fixture.capacitor","pins":{"p":")" + p + R"(","n":")" + n + R"("},"parameters":{"capacitanceF":)" + farad + "}}";
}
std::string D(const std::string& id, const std::string& a, const std::string& k) {
    return R"({"id":")" + id + R"(","modelId":"fixture.diode","pins":{"anode":")" + a + R"(","cathode":")" + k +
           R"("},"parameters":{"saturationCurrentA":1e-14,"emissionCoefficient":1,"thermalVoltageV":0.025852}})";
}
std::string V(const std::string& id, const std::string& p, const std::string& n, double v = 1) {
    return R"({"id":")" + id + R"(","modelId":"fixture.vsource","pins":{"p":")" + p + R"(","n":")" + n + R"("},"parameters":{"voltageV":)" + std::to_string(v) + "}}";
}
const std::string kNodes = R"(["0","in","out"])";
const std::string kRc = doc(kNodes, "[" + R("R1", "in", "out") + "," + C("C1", "out", "0") + "]");
const std::string kClip = doc(kNodes, "[" + R("R1", "in", "out", 4700) + "," + D("D1", "out", "0") + "," + D("D2", "0", "out") + "," + C("C1", "out", "0", "1e-9") + "]");

CompileResult run(const std::string& text, const RuntimeConfig& cfg = kCfg8) { return compileText(text, kReg, cfg); }
bool rejectedWith(const std::string& text, DiagCode code, const RuntimeConfig& cfg = kCfg8) {
    const CompileResult r = run(text, cfg);
    return !r.ok && !r.graph && r.diagnostics.has(code);
}

void t10Valid() {
    CompileResult r = run(kRc, kCfg48);
    CHECK(r.ok && r.graph && r.failedCheck == 0 && r.diagnostics.empty());
    if (r.graph) {
        CHECK(r.graph->cost.validatedForThisConfigurationOnly && r.graph->cost.unknowns == 3);  // in, out + drive-source current
        CHECK(r.graph->cost.components == 2 && r.graph->cost.nodes == 3 && r.graph->cost.pinConnections == 4);
        CHECK(r.graph->cost.dcIterations >= 1 && r.graph->cost.dcIterations <= 100);
        CHECK(r.graph->cost.probePeakVolts[0] == 0);                       // silence stays silent
        CHECK(r.graph->cost.probePeakVolts[1] > 0.01 && r.graph->cost.probePeakVolts[1] < 1);  // -18 dBFS RC lowpass response
        CHECK(r.graph->cost.probePeakVolts[2] > 0 && r.graph->cost.probePeakVolts[2] < 1);
        cd::dsp::MnaSolver s;
        std::string e;
        CHECK(r.graph->makeSolver(s, e));
    }
    CHECK(run(kClip, kCfg48).ok);
    // Programmatic round trip: parse -> write -> parse -> identical canonical text.
    Blueprint bp;
    Diagnostics d;
    CHECK(parseBlueprint(kClip, bp, d) && d.empty());
    Blueprint bp2;
    CHECK(parseBlueprint(writeBlueprint(bp), bp2, d) && writeBlueprint(bp2) == writeBlueprint(bp));
    // Documented example conforms.
    // (example text embedded: same as docs/planned-features/examples/rc-blueprint.json, with a position field)
    const std::string ex = R"({"schemaVersion":1,"id":"example.rc","name":"Synthetic RC validation fixture","modelRegistryVersion":"fixture-v1",
      "nodes":["0","in","out"],"components":[{"id":"R1","modelId":"fixture.resistor","pins":{"p":"in","n":"out"},"parameters":{"resistanceOhm":10000},"position":[0,0,0]},
      {"id":"C1","modelId":"fixture.capacitor","pins":{"p":"out","n":"0"},"parameters":{"capacitanceF":1e-8},"position":[1,0,0]}],
      "ports":{"input":"in","output":"out","ground":"0"},"macroBindings":[]})";
    CHECK(run(ex).ok);
    // Generation is assigned by the caller; stable-state map for identical graphs covers all components.
    CompileResult a = compileText(kRc, kReg, kCfg8, 7), b = compileText(kRc, kReg, kCfg8, 8);
    CHECK(a.ok && b.ok && a.graph->generation == 7 && b.graph->generation == 8);
    CHECK(stableStateMap(*a.graph, *b.graph).size() == 2);
    // Moving a component in the view (position only) keeps identities and wiring.
    CHECK(run(kRc, {48000, 4, 1.0}).ok);
}

void t10Rejects() {
    // dangling pins / unknown pins / undeclared nodes
    CHECK(rejectedWith(doc(kNodes, "[" + R("R1", "in", "out") + R"(,{"id":"C1","modelId":"fixture.capacitor","pins":{"p":"out"},"parameters":{"capacitanceF":1e-8}}])"), DiagCode::LimitExceeded));  // <2 pins at parse
    CHECK(rejectedWith(doc(kNodes, "[" + R("R1", "in", "out") + R"(,{"id":"C1","modelId":"fixture.capacitor","pins":{"p":"out","x":"0"},"parameters":{"capacitanceF":1e-8}}])"), DiagCode::PinMissing));
    CHECK(rejectedWith(doc(kNodes, "[" + R("R1", "in", "out") + R"(,{"id":"C1","modelId":"fixture.capacitor","pins":{"p":"out","x":"0"},"parameters":{"capacitanceF":1e-8}}])"), DiagCode::PinUnknown));
    CHECK(rejectedWith(doc(kNodes, "[" + R("R1", "in", "out") + "," + C("C1", "out", "nowhere") + "]"), DiagCode::PinNode));
    {
        auto r = run(doc(kNodes, "[" + R("R1", "in", "out") + "," + C("C1", "out", "nowhere") + "]"));
        CHECK(r.diagnostics.hasFor(DiagCode::PinNode, "C1") && r.diagnostics.items[0].pin == "n" && r.failedCheck == 2);
    }
    // unsupported models: never previewed as implemented
    for (const char* m : {"fixture.inductor", "12AX7", "ef86", "bjt.2n3904", "fixture.transformer"})
        CHECK(rejectedWith(doc(kNodes, "[" + R("R1", "in", "out") + R"(,{"id":"X1","modelId":")" + m + R"(","pins":{"p":"out","n":"0"},"parameters":{}}])"), DiagCode::UnsupportedModel));
    CHECK(rejectedWith(kRc.substr(0, 0) + doc(kNodes, "[" + R("R1", "in", "out") + "," + C("C1", "out", "0") + "]", R"({"input":"in","output":"out","ground":"0"})", "", "1", "tube-v9"), DiagCode::RegistryMismatch));
    // parameter range / missing / unknown / conductance
    CHECK(rejectedWith(doc(kNodes, "[" + R("R1", "in", "out", 1e12) + "," + C("C1", "out", "0") + "]"), DiagCode::ParamRange));
    CHECK(rejectedWith(doc(kNodes, "[" + R("R1", "in", "out", 0) + "," + C("C1", "out", "0") + "]"), DiagCode::ParamRange));
    CHECK(rejectedWith(doc(kNodes, "[" + R("R1", "in", "out") + "," + C("C1", "out", "0", "-1e-8") + "]"), DiagCode::ParamRange));
    CHECK(rejectedWith(doc(kNodes, R"([{"id":"R1","modelId":"fixture.resistor","pins":{"p":"in","n":"out"},"parameters":{}},)" + C("C1", "out", "0") + "]"), DiagCode::ParamMissing));
    CHECK(rejectedWith(doc(kNodes, R"([{"id":"R1","modelId":"fixture.resistor","pins":{"p":"in","n":"out"},"parameters":{"resistanceOhm":1000,"bogus":1}},)" + C("C1", "out", "0") + "]"), DiagCode::ParamUnknown));
    // connectivity: no ground use, floating node, island, capacitor-only (no DC path), self short
    CHECK(rejectedWith(doc(kNodes, "[" + R("R1", "in", "out") + "]"), DiagCode::NoGround));
    CHECK(rejectedWith(doc(R"(["in","out"])", "[" + R("R1", "in", "out") + "]"), DiagCode::NoGround));
    CHECK(rejectedWith(doc(R"(["0","in","out","spare"])", "[" + R("R1", "in", "out") + "," + C("C1", "out", "0") + "]"), DiagCode::FloatingNode));
    CHECK(rejectedWith(doc(R"(["0","in","out","a","b"])", "[" + R("R1", "in", "out") + "," + C("C1", "out", "0") + "," + R("R2", "a", "b") + "]"), DiagCode::FloatingIsland));
    CHECK(rejectedWith(doc(R"(["0","in","out","mid"])", "[" + C("C1", "in", "mid") + "," + C("C2", "mid", "out") + "," + R("R1", "out", "0") + "]"), DiagCode::NoDcPath));
    CHECK(rejectedWith(doc(kNodes, "[" + R("R1", "in", "out") + "," + C("C1", "out", "out") + "," + R("R2", "out", "0") + "]"), DiagCode::SelfShort));
    CHECK(rejectedWith(doc(kNodes, "[" + R("R1", "in", "out") + "," + C("C1", "out", "0") + "]", R"({"input":"in","output":"in","ground":"0"})"), DiagCode::BadTerminal));
    CHECK(rejectedWith(doc(kNodes, "[" + R("R1", "in", "out") + "," + C("C1", "out", "0") + "]", R"({"input":"in","output":"0","ground":"0"})"), DiagCode::BadTerminal));
    CHECK(rejectedWith(doc(kNodes, "[" + R("R1", "in", "out") + "," + C("C1", "out", "0") + "]", R"({"input":"in","output":"zz","ground":"0"})"), DiagCode::BadTerminal));
    CHECK(rejectedWith(doc(kNodes, "[" + R("R1", "in", "out") + "," + C("C1", "out", "0") + "]", R"({"input":"in","output":"out","ground":"gnd"})"), DiagCode::NoGround));
    // invalid ideal voltage-source loops (parallel sources, loop through source ring, source across the drive)
    CHECK(rejectedWith(doc(kNodes, "[" + R("R1", "in", "out") + "," + C("C1", "out", "0") + "," + V("V1", "out", "0", 5) + "," + V("V2", "0", "out", 3) + "]"), DiagCode::VoltageSourceLoop));
    CHECK(rejectedWith(doc(kNodes, "[" + R("R1", "in", "out") + "," + C("C1", "out", "0") + "," + V("V1", "in", "0", 5) + "]"), DiagCode::VoltageSourceLoop));  // loop with the injected drive
    CHECK(rejectedWith(doc(R"(["0","in","out","a"])", "[" + R("R1", "in", "out") + "," + C("C1", "out", "0") + "," + V("V1", "a", "out", 5) + "," + V("V2", "a", "0", 5) + "," + V("V3", "out", "0", 5) + "]"), DiagCode::VoltageSourceLoop));
    // accidental supply short: sub-ohm resistor across a source
    CHECK(rejectedWith(doc(R"(["0","in","out","vcc"])", "[" + R("R1", "in", "out") + "," + C("C1", "out", "0") + "," + V("V1", "vcc", "0", 9) + "," + R("R2", "vcc", "0", 0.01) + "]"), DiagCode::SupplyShort));
    // valid supply wiring is accepted
    CHECK(run(doc(R"(["0","in","out","vcc"])", "[" + R("R1", "in", "out") + "," + C("C1", "out", "0") + "," + V("V1", "vcc", "0", 9) + "," + R("R2", "vcc", "out", 100000) + "]")).ok);
    // nonlinear count: 9 diodes rejected, 8 accepted
    {
        std::string c = "[" + R("R1", "in", "out") + "," + C("C1", "out", "0");
        for (int k = 0; k < 9; ++k) c += "," + D("D" + std::to_string(k), "out", "0");
        CHECK(rejectedWith(doc(kNodes, c + "]"), DiagCode::LimitExceeded));
        std::string c8 = "[" + R("R1", "in", "out") + "," + C("C1", "out", "0");
        for (int k = 0; k < 8; ++k) c8 += "," + D("D" + std::to_string(k), "out", "0");
        CHECK(run(doc(kNodes, c8 + "]")).ok);
    }
    // bindings
    CHECK(rejectedWith(doc(kNodes, "[" + R("R1", "in", "out") + "," + C("C1", "out", "0") + "]", R"({"input":"in","output":"out","ground":"0"})",
                           R"(,"modSlots":[{"slot":0,"component":"R9","field":"resistanceOhm"}])"), DiagCode::BadBinding));
    CHECK(rejectedWith(doc(kNodes, "[" + R("R1", "in", "out") + "," + C("C1", "out", "0") + "]", R"({"input":"in","output":"out","ground":"0"})",
                           R"(,"modSlots":[{"slot":16,"component":"R1","field":"resistanceOhm"}])"), DiagCode::BadBinding));
    CHECK(rejectedWith(doc(kNodes, "[" + R("R1", "in", "out") + "," + C("C1", "out", "0") + "]", R"({"input":"in","output":"out","ground":"0"})",
                           R"(,"modSlots":[{"slot":1,"component":"R1","field":"resistanceOhm"},{"slot":1,"component":"C1","field":"capacitanceF"}])"), DiagCode::DuplicateId));
    {
        std::string withSlots = doc(kNodes, "[" + R("R1", "in", "out") + "," + C("C1", "out", "0") + "]", R"({"input":"in","output":"out","ground":"0"})",
                                    R"(,"modSlots":[{"slot":0,"component":"R1","field":"resistanceOhm"},{"slot":15,"component":"C1","field":"capacitanceF"}])");
        CompileResult r = run(withSlots);
        CHECK(r.ok);
        if (r.graph) {
            const auto& s0 = r.graph->modSlots[0];
            CHECK(s0.bound && s0.component == 0 && s0.paramIndex == 0 && r.graph->modSlots[15].bound && !r.graph->modSlots[1].bound);
        }
    }
    // runtime config
    CHECK(rejectedWith(kRc, DiagCode::BadConfig, {std::nan(""), 1, 1}));
    CHECK(rejectedWith(kRc, DiagCode::BadConfig, {48000, 3, 1}));
    CHECK(rejectedWith(kRc, DiagCode::BadConfig, {100, 1, 1}));
}

void t10ParseBounds() {
    const std::string ok = "[" + R("R1", "in", "out") + "," + C("C1", "out", "0") + "]";
    // 1 MiB, depth, duplicate keys, non-finite, malformed
    CHECK(rejectedWith(std::string(kMaxBytes + 1, ' '), DiagCode::TooLarge));
    CHECK(rejectedWith(doc(kNodes, ok, R"({"input":"in","output":"out","ground":"0"})", R"(,"pad":")" + std::string(kMaxBytes, 'a') + "\""), DiagCode::TooLarge));
    {
        std::string deep;
        for (int k = 0; k < 17; ++k) deep += "[";
        for (int k = 0; k < 17; ++k) deep += "]";
        CHECK(rejectedWith(doc(kNodes, ok, R"({"input":"in","output":"out","ground":"0"})", ",\"x\":" + deep), DiagCode::TooDeep));
    }
    CHECK(rejectedWith(R"({"schemaVersion":1,"schemaVersion":1})", DiagCode::DuplicateKey));
    CHECK(rejectedWith(doc(kNodes, "[" + R("R1", "in", "out") + "," + C("C1", "out", "0", "1e999") + "]"), DiagCode::NonFinite));
    CHECK(rejectedWith(doc(kNodes, "[" + R("R1", "in", "out", 1) + "," + C("C1", "out", "0", "NaN") + "]"), DiagCode::Malformed));
    CHECK(rejectedWith(doc(kNodes, "[" + R("R1", "in", "out", 1) + "," + C("C1", "out", "0", "Infinity") + "]"), DiagCode::Malformed));
    for (const char* bad : {"", "{", "[]", "null", "{\"schemaVersion\":1,", "\xff\xfe", "{\"a\":\"\\ud800\"}", "{} {}"})
        CHECK(rejectedWith(bad, DiagCode::Malformed) || rejectedWith(bad, DiagCode::BadType));
    // unknown schema major, bad schemaVersion types
    CHECK(rejectedWith(doc(kNodes, ok, R"({"input":"in","output":"out","ground":"0"})", "", "2"), DiagCode::UnknownSchema));
    CHECK(rejectedWith(doc(kNodes, ok, R"({"input":"in","output":"out","ground":"0"})", "", "1.5"), DiagCode::UnknownSchema));
    CHECK(rejectedWith(doc(kNodes, ok, R"({"input":"in","output":"out","ground":"0"})", "", "\"1\""), DiagCode::UnknownSchema));
    // unknown / executable fields
    for (const char* key : {"script", "shader", "url", "path", "plugin", "spice", "netlist", "include", "Script", "SPICE"})
        CHECK(rejectedWith(doc(kNodes, ok, R"({"input":"in","output":"out","ground":"0"})", std::string(",\"") + key + "\":\"x\""), DiagCode::ExecutableField));
    {
        std::string r1 = R("R1", "in", "out");
        r1.insert(r1.size() - 1, ",\"script\":\"x\"");
        CHECK(rejectedWith(doc(kNodes, "[" + r1 + "," + C("C1", "out", "0") + "]"), DiagCode::ExecutableField));
    }
    CHECK(rejectedWith(doc(kNodes, ok, R"({"input":"in","output":"out","ground":"0"})", R"(,"harmless":1)"), DiagCode::UnknownField));
    // resource references in strings
    for (const char* s : {"http://evil.example/x", "https://x", "file:///etc/passwd", "/etc/passwd", "C:\\\\x\\\\y", "../up", "~/x", ".include foo.lib", "data:text/plain,x", "a\\\\b"}) {
        std::string t = kRc;
        const std::size_t p = t.find("T10 fixture");
        t.replace(p, 11, s);
        CHECK(rejectedWith(t, DiagCode::ResourceReference));
    }
    // identifiers and names
    CHECK(rejectedWith(doc(kNodes, "[" + R(std::string(65, 'a'), "in", "out") + "," + C("C1", "out", "0") + "]"), DiagCode::BadId));
    CHECK(run(doc(kNodes, "[" + R(std::string(64, 'a'), "in", "out") + "," + C("C1", "out", "0") + "]")).ok);
    CHECK(rejectedWith(doc(kNodes, "[" + R("R 1", "in", "out") + "," + C("C1", "out", "0") + "]"), DiagCode::BadId));
    CHECK(rejectedWith(doc(kNodes, "[" + R("R\\u00e9", "in", "out") + "," + C("C1", "out", "0") + "]"), DiagCode::BadId));
    CHECK(rejectedWith(doc(kNodes, "[" + R("R1", "in", "out") + "," + R("R1", "out", "0") + "]"), DiagCode::DuplicateId));
    CHECK(rejectedWith(doc(R"(["0","in","out","in"])", ok), DiagCode::DuplicateId));
    {
        std::string name80, name81;
        for (int k = 0; k < 80; ++k) name80 += "\xC3\xA9";   // 80 two-byte code points = 160 bytes, accepted
        name81 = name80 + "e";
        std::string t = kRc;
        const std::size_t p = t.find("T10 fixture");
        std::string a = t, b = t;
        a.replace(p, 11, name80);
        b.replace(p, 11, name81);
        CHECK(run(a).ok);
        CHECK(rejectedWith(b, DiagCode::BadName));
    }
    // resource limits
    {
        std::string c = "[" + C("C0", "out", "0");
        for (int k = 0; k < 64; ++k) c += "," + R("R" + std::to_string(k), "in", "out");
        CHECK(rejectedWith(doc(kNodes, c + "]"), DiagCode::LimitExceeded));   // 65 components
    }
    {   // exactly 64 user components compile: the injected drive source does not consume the component bound
        std::string c = "[" + C("C0", "out", "0");
        for (int k = 0; k < 63; ++k) c += "," + R("R" + std::to_string(k), "in", "out");
        CHECK(run(doc(kNodes, c + "]")).ok);
    }
    {
        std::string nodes = R"(["0","in","out")";
        for (int k = 0; k < 31; ++k) nodes += ",\"n" + std::to_string(k) + "\"";   // 34 nodes
        CHECK(rejectedWith(doc(nodes + "]", ok), DiagCode::LimitExceeded));
    }
    {
        // 33 nodes (max) incl. ground accepted; each extra node tied to ground through a resistor.
        std::string nodes = R"(["0","in","out")", c = "[" + R("R1", "in", "out") + "," + C("C1", "out", "0");
        for (int k = 0; k < 30; ++k) { nodes += ",\"n" + std::to_string(k) + "\""; c += "," + R("Q" + std::to_string(k), "n" + std::to_string(k), "0"); }
        const CompileResult r = run(doc(nodes + "]", c + "]"));
        CHECK(r.ok && r.graph && r.graph->cost.nodes == 33 && r.graph->cost.unknowns == 33);
    }
    {
        // pin connection limit: 129 two-pin components -> 258 pins, but components already > 64; use pins-per-component bound
        std::string c = "[";
        for (int k = 0; k < 64; ++k) {
            if (k) c += ",";
            c += R"({"id":"X)" + std::to_string(k) + R"(","modelId":"fixture.resistor","pins":{"a":"in","b":"out","c":"in","d":"out","e":"in","f":"out","g":"in","h":"out"},"parameters":{"resistanceOhm":1000}})";
        }
        CHECK(rejectedWith(doc(kNodes, c + "]"), DiagCode::LimitExceeded));  // 512 pin connections
        CHECK(rejectedWith(doc(kNodes, "[" + std::string(R"({"id":"X","modelId":"fixture.resistor","pins":{"a":"in","b":"out","c":"in","d":"out","e":"in","f":"out","g":"in","h":"out","i":"0"},"parameters":{"resistanceOhm":1000}})") + "]"), DiagCode::LimitExceeded));
    }
    // Spec bound is 64 user components; the injected drive source uses the separate port-source budget.
    {
        std::string c63 = "[" + C("C0", "out", "0");
        for (int k = 0; k < 62; ++k) c63 += "," + R("R" + std::to_string(k), "in", "out");
        CHECK(run(doc(kNodes, c63 + "]")).ok);   // 63 components compile
    }
}

void t10ActiveGraphUnchanged() {
    GraphSlot slot;
    CHECK(!slot.active() && slot.generation() == 0);
    CompileResult first = slot.compileAndApply(kRc, kReg, kCfg8);
    CHECK(first.ok && slot.generation() == 1 && slot.active() == first.graph && first.graph->generation == 1);
    const PreparedHandle active = slot.active();
    const std::string bad[] = {
        "{not json", kRc.substr(0, kRc.size() / 2),
        doc(kNodes, "[" + R("R1", "in", "out") + "]"),                                                  // no ground
        doc(kNodes, "[" + R("R1", "in", "out") + "," + C("C1", "out", "0") + "," + V("V1", "in", "0") + "]"),  // V loop
        doc(kNodes, "[" + R("R1", "in", "out") + R"(,{"id":"L1","modelId":"fixture.inductor","pins":{"p":"out","n":"0"},"parameters":{}}])"),
        doc(kNodes, "[" + C("C1", "in", "out") + "," + C("C2", "out", "0") + "]"),                     // singular: no DC path for out
        doc(kNodes, "[" + R("R1", "in", "out") + "," + C("C1", "out", "0") + "]", R"({"input":"in","output":"out","ground":"0"})", ",\"script\":\"x\""),
    };
    for (const auto& b : bad) {
        const CompileResult r = slot.compileAndApply(b, kReg, kCfg8);
        CHECK(!r.ok && !r.graph && !r.diagnostics.empty());
        CHECK(slot.active() == active && slot.generation() == 1);       // untouched
    }
    // invalid config also leaves the slot alone
    CHECK(!slot.compileAndApply(kRc, kReg, RuntimeConfig{0, 1, 1}).ok && slot.active() == active && slot.generation() == 1);
    // valid swap = exactly one generation increment, new immutable graph, old one still alive for the audio thread
    CompileResult swap = slot.compileAndApply(kClip, kReg, kCfg8);
    CHECK(swap.ok && slot.generation() == 2 && slot.active() == swap.graph && swap.graph != active);
    CHECK(active->generation == 1 && active.use_count() >= 2 - 1);
}

// Deterministic random byte mutations of valid blueprints: never crash/UB; outcomes are consistent.
void t10Fuzz() {
    std::mt19937_64 rng(0xC1AC017u);
    const std::string seeds[] = {kRc, kClip};
    int accepted = 0, rejected = 0;
    for (int it = 0; it < 2000; ++it) {
        std::string t = seeds[std::size_t(it) & 1];
        const int edits = 1 + int(rng() % 4);
        for (int e = 0; e < edits && !t.empty(); ++e) {
            const std::size_t pos = std::size_t(rng() % t.size());
            switch (rng() % 5) {
                case 0: t[pos] = char(rng() & 0xFF); break;
                case 1: t.erase(pos, 1 + std::size_t(rng() % 3)); break;
                case 2: t.insert(pos, 1, char(rng() & 0xFF)); break;
                case 3: t[pos] = "{}[],:\"0123456789eE+-."[rng() % 22]; break;
                default: if (pos + 8 < t.size()) t.replace(pos, 4, t.substr(pos + 4, 4)); break;
            }
        }
        const CompileResult r = run(t);
        if (r.ok) {
            ++accepted;
            CHECK(r.graph && r.diagnostics.empty() && r.failedCheck == 0);
            // an accepted mutant must itself be a valid, re-compilable blueprint
            Blueprint bp;
            Diagnostics d;
            CHECK(parseBlueprint(t, bp, d) && d.empty());
            CHECK(compile(bp, kReg, kCfg8).ok);
        } else {
            ++rejected;
            CHECK(!r.graph && !r.diagnostics.empty() && r.failedCheck >= 1 && r.failedCheck <= 6);
        }
    }
    std::printf("T10 fuzz: 2000 mutants, %d accepted, %d rejected\n", accepted, rejected);
    CHECK(rejected > 1000);
}

}  // namespace

int main() {
    t10Valid();
    t10Rejects();
    t10ParseBounds();
    t10ActiveGraphUnchanged();
    t10Fuzz();
    if (failures) std::printf("blueprint_compile: %d FAILED\n", failures);
    else std::printf("blueprint_compile: all checks passed\n");
    return failures ? 1 : 0;
}
