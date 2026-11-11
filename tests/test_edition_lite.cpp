// CircuitDrift tests: T05 / P12 LITE edition (07-lite-edition). The LITE topology is the SYNTHETIC
// fixture-v1 diode clipper; nothing here validates hardware (G01). Format identifiers are PROPOSED
// placeholders (G08/G09). Owner: integration agent.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <set>
#include <span>
#include <string>
#include <vector>
#include "core/ParameterRegistry.h"
#include "edition/EditionManifest.h"
#include "edition/LiteEngine.h"
#include "edition/LiteState.h"
#include "runtime/Engine.h"
#include "state/StateDocument.h"
#include "state/TransactionQueue.h"
#include "support/RtTrap.h"

namespace {
using namespace cd;
using namespace cd::edition;
using cd::state::TransactionEvent;
int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)
long g_allocs = 0;

struct Lcg {
    uint64_t s;
    uint32_t u() { s = s * 6364136223846793005ull + 1442695040888963407ull; return uint32_t(s >> 33); }
    double d() { return double(u()) / 2147483648.0 - 1.0; }
    double unit() { return double(u()) / 4294967296.0; }
};
using Buf = std::vector<std::vector<double>>;

Buf seededInput(std::size_t n, double fs, uint64_t seed) {
    Buf x(2, std::vector<double>(n));
    Lcg g{seed};
    for (std::size_t i = 0; i < n; ++i) {
        const double t = double(i) / fs;
        x[0][i] = 0.5 * std::sin(2 * M_PI * 220 * t) + 0.25 * g.d();
        x[1][i] = 0.4 * std::sin(2 * M_PI * 3300 * t) + 0.2 * g.d();
    }
    return x;
}

struct Ev { std::size_t t; uint32_t id; double v; };

// Generic block renderer: `proc(ctx, buses, span)` with per-block events at relative offsets.
template <class Proc>
Buf render(Proc&& proc, const Buf& in, double fs, uint32_t block, const std::vector<Ev>& evs) {
    const std::size_t N = in[0].size();
    Buf out(2, std::vector<double>(N));
    std::vector<TransactionEvent> blockEvents;
    blockEvents.reserve(256);
    std::size_t pos = 0, k = 0;
    while (pos < N) {
        const uint32_t f = uint32_t(std::min<std::size_t>(block, N - pos));
        blockEvents.clear();
        while (k < evs.size() && evs[k].t < pos + f) { blockEvents.push_back(TransactionEvent::param(evs[k].id, evs[k].v, uint32_t(evs[k].t - pos))); ++k; }
        const double* ip[2] = {in[0].data() + pos, in[1].data() + pos};
        double* op[2] = {out[0].data() + pos, out[1].data() + pos};
        {
            cd::test::RtTrapScope trap;
            proc(runtime::ProcessContext{fs, int64_t(pos), 0, f, false}, runtime::AudioBuses{2, ip, op},
                 std::span<const TransactionEvent>(blockEvents.data(), blockEvents.size()));
            g_allocs += trap.counts().allocations + trap.counts().deallocations;
        }
        pos += f;
    }
    return out;
}

double peakDiff(const Buf& a, const Buf& b) {
    double m = 0;
    for (std::size_t c = 0; c < a.size(); ++c)
        for (std::size_t i = 0; i < a[c].size(); ++i) m = std::max(m, std::fabs(a[c][i] - b[c][i]));
    return m;
}
bool finite(const Buf& a) { for (const auto& c : a) for (double v : c) if (!std::isfinite(v)) return false; return true; }

// Equivalent full configuration, built directly from runtime/edition primitives (not via LiteEngine).
bool fullEquivalent(const LiteKnobs& k, double fs, runtime::Engine& eng, std::string& e) {
    runtime::QualitySettings q;
    q.mode = runtime::QualityMode::Eco;
    PreparedEngineConfig c;
    if (!circuitFromBlueprint(liteFactoryBlueprintJson(), fs, 512, 2, q, c, e)) return false;
    c.engine.initial = runtime::OutputParams{0.0, 0.0, 1.0, true, true, 15.0, true, false};
    c.engine.initialMacros = control::MacroValues{k.thermalAge, k.sag, 0.25 * k.thermalAge, 0.2};
    return eng.prepare(c.engine, e);
}

// LITE automation and its full-engine macro equivalent.
std::vector<Ev> liteAutomation(std::size_t n, uint64_t seed) {
    Lcg g{seed};
    std::vector<Ev> ev;
    for (int i = 0; i < 24; ++i) ev.push_back({std::size_t(g.u() % n), (g.u() & 1) ? kLiteSagHostId : kLiteThermalAgeHostId, g.unit()});
    std::stable_sort(ev.begin(), ev.end(), [](const Ev& a, const Ev& b) { return a.t < b.t; });
    return ev;
}
std::vector<Ev> toMacroEvents(const std::vector<Ev>& lite) {
    std::vector<Ev> m;
    for (const Ev& e : lite) {
        if (e.id == kLiteSagHostId) m.push_back({e.t, 1001, e.v});
        else { m.push_back({e.t, 1000, e.v}); m.push_back({e.t, 1002, 0.25 * e.v}); }
    }
    return m;
}

auto liteProc(LiteEngine& l) {
    return [&l](const runtime::ProcessContext& c, const runtime::AudioBuses& b, std::span<const TransactionEvent> e) { l.process(c, b, e); };
}
auto fullProc(runtime::Engine& f) {
    return [&f](const runtime::ProcessContext& c, const runtime::AudioBuses& b, std::span<const TransactionEvent> e) { f.process(c, b, e); };
}

// ---------------------------------------------------------------------------------------------
void manifestAndIdentity() {
    std::string why;
    CHECK(identitiesDistinct(why), "identities collide: %s", why.c_str());
    CHECK(std::string(kIdentityStatus).find("PROPOSED") != std::string::npos, "identity status must say PROPOSED");
    std::set<std::string> ids;
    for (Edition e : {Edition::Full, Edition::Lite})
        for (const auto& f : editionIdentity(e).formats) {
            ids.insert(f.pluginId); ids.insert(f.bundleId);
            const std::string pid = f.pluginId, bid = f.bundleId;
            CHECK((pid.rfind("PLACEHOLDER-", 0) == 0 || pid.rfind("invalid.placeholder.", 0) == 0) && bid.rfind("invalid.placeholder.", 0) == 0,
                  "identifier not labelled as placeholder: %s / %s", f.pluginId, f.bundleId);
        }
    CHECK(ids.size() == 16, "expected 16 distinct identifiers, got %zu", ids.size());
    const EditionManifest& lite = editionManifest(Edition::Lite);
    CHECK(lite.primaryControls.size() == 2 && lite.primaryControls[0] == 1040 && lite.primaryControls[1] == 1041, "LITE primary controls");
    CHECK(lite.liteMappingVersion == 1 && !lite.graphEditing && !lite.faults && !lite.ipc && !lite.communityPublishing &&
          !lite.abDualEngines && !lite.mediaRecorder && !lite.workbench3d, "LITE feature surface");
    CHECK(core::findByHostId(1040)->def == kLiteDefaultSag && core::findByHostId(1041)->def == kLiteDefaultThermalAge, "registry defaults");
    std::size_t liteExposed = 0, fullExposed = 0;
    for (const auto& p : core::kParams) {
        if (exposes(Edition::Lite, p.hostId)) { ++liteExposed; CHECK(p.hostId == 1040 || p.hostId == 1041, "LITE leaks %s", p.id); }
        if (exposes(Edition::Full, p.hostId)) ++fullExposed;
    }
    CHECK(liteExposed == 2 && fullExposed == core::kParamCount - 2 && !exposes(Edition::Full, 1040) && !exposes(Edition::Lite, 4242),
          "exposure lite %zu full %zu", liteExposed, fullExposed);
    std::printf("T05 identity/manifest: 16 distinct PROPOSED placeholder IDs (VST3/AU/AAX/CLAP x full/LITE); LITE exposes exactly 2 of %zu "
                "registry parameters (lite.sag, lite.thermalAge)\n", core::kParamCount);
}

void mappingEndpoints() {
    for (const LiteKnobs k : {LiteKnobs{0, 0}, LiteKnobs{1, 1}, LiteKnobs{0, 1}, LiteKnobs{1, 0}, LiteKnobs{0.25, 0.2}}) {
        const LiteMapped m = mapLite(k);
        CHECK(m.macros.sag == k.sag && m.macros.age == k.thermalAge && m.macros.drift == 0.25 * k.thermalAge && m.macros.warmth == 0.2,
              "mapping at (%g,%g)", k.sag, k.thermalAge);
        CHECK(m.output.mix == 1 && m.output.autoGain && m.output.inputGainDb == 0 && m.output.outputTrimDb == 0 && m.output.hpfEnabled &&
              m.output.hpfCutoffHz == 15 && m.output.softClipEnabled && !m.analogNoise && !m.pitchDelay &&
              m.quality.mode == runtime::QualityMode::Eco && m.quality.family == dsp::OsFamily::Fir, "fixed LITE controls");
    }
    const LiteMapped bad = mapLite(LiteKnobs{std::nan(""), 7.0});
    CHECK(bad.macros.sag == kLiteDefaultSag && bad.macros.age == 1.0, "non-finite/out-of-range knobs");
}

void t05Render() {
    std::string e;
    for (double fs : {48000.0, 96000.0}) {
        const std::size_t N = std::size_t(fs * 0.25);
        const Buf in = seededInput(N, fs, 99);
        LiteState st;
        LiteEngine lite;
        CHECK(lite.prepare(st, fs, 512, 2, e), "LITE prepare %s", e.c_str());
        runtime::Engine full;
        CHECK(fullEquivalent(st.knobs, fs, full, e), "full prepare %s", e.c_str());
        CHECK(lite.latencySamples() == full.latencySamples() && lite.engine().quality().factor == 4 && lite.engine().quality().maxNewton == 6,
              "latency/quality mismatch");
        const auto autom = liteAutomation(N, uint64_t(fs));
        const Buf yl = render(liteProc(lite), in, fs, 256, autom);
        const Buf yf = render(fullProc(full), in, fs, 256, toMacroEvents(autom));
        const double d = peakDiff(yl, yf);   // equal declared latency: aligned sample for sample
        CHECK(finite(yl) && d <= 1e-6, "T05 LITE vs full at %.0f Hz: %.3e", fs, d);
        CHECK(lite.diagnostics().accepted == 24 && lite.diagnostics().rejected == 0, "automation accepted %lld",
              (long long)lite.diagnostics().accepted);
        // Upgrade export -> full scene text -> full import -> identical render.
        state::StateDocument doc;
        CHECK(exportLiteUpgrade(st, "2026-11-04T00:00:00Z", doc, e), "export %s", e.c_str());
        std::string text;
        CHECK(state::saveState(doc, text, e), "save scene %s", e.c_str());
        state::StateDocument loaded;
        CHECK(state::loadState(text, loaded, e), "full load %s", e.c_str());
        SceneEngineConfig sc;
        CHECK(sceneToEngineConfig(loaded, fs, 512, 2, sc, e), "scene import %s", e.c_str());
        runtime::Engine upgraded;
        CHECK(upgraded.prepare(sc.prepared.engine, e), "upgraded prepare %s", e.c_str());
        LiteEngine lite2;
        CHECK(lite2.prepare(st, fs, 512, 2, e), "prepare");
        const Buf a = render(liteProc(lite2), in, fs, 512, {}), b = render(fullProc(upgraded), in, fs, 512, {});
        const double du = peakDiff(a, b);
        CHECK(du <= 1e-6 && upgraded.latencySamples() == lite2.latencySamples(), "upgrade export render differs %.3e", du);
        // Informational: the same fixture via the runtime's built-in netlist (element order differs).
        runtime::EngineConfig bi;
        bi.sampleRate = fs; bi.maxBlock = 512; bi.channels = 2; bi.quality.mode = runtime::QualityMode::Eco;
        bi.circuit = runtime::syntheticDiodeClipper();
        bi.circuit.supply = runtime::syntheticFactorySupply();   // same reduced synthetic supply as the factory topology

        bi.initial = runtime::OutputParams{0.0, 0.0, 1.0, true, true, 15.0, true, false};
        bi.initialMacros = control::MacroValues{st.knobs.thermalAge, st.knobs.sag, 0.25 * st.knobs.thermalAge, 0.2};
        runtime::Engine builtin;
        CHECK(builtin.prepare(bi, e), "builtin prepare");
        const double dbi = peakDiff(b, render(fullProc(builtin), in, fs, 512, {}));
        std::printf("  (info) blueprint-compiled factory topology vs runtime built-in clipper netlist: peak %.3e\n", dbi);
        std::printf("T05 render @%.0f Hz (seeded stereo, 24 LITE automation events, latency %d both): LITE vs equivalent full peak %.3e; "
                    "upgrade-export scene in full engine vs LITE peak %.3e (limit 1e-6)\n", fs, lite.latencySamples(), d, du);
    }
}

void knobEndpoints() {
    std::string e;
    const double fs = 48000;
    const Buf in = seededInput(4800, fs, 5);
    double worst = 0;
    for (const LiteKnobs k : {LiteKnobs{0, 0}, LiteKnobs{1, 1}, LiteKnobs{0, 1}, LiteKnobs{1, 0}}) {
        LiteState st;
        st.knobs = k;
        LiteEngine lite;
        runtime::Engine full;
        CHECK(lite.prepare(st, fs, 512, 2, e) && fullEquivalent(k, fs, full, e), "prepare %s", e.c_str());
        const Buf yl = render(liteProc(lite), in, fs, 480, {}), yf = render(fullProc(full), in, fs, 480, {});
        const auto d = lite.engine().diagnostics();
        CHECK(d.macros.age == k.thermalAge && d.macros.sag == k.sag && d.macros.drift == 0.25 * k.thermalAge && d.macros.warmth == 0.2,
              "effective macros at (%g,%g)", k.sag, k.thermalAge);
        // Factory topology carries the reduced SYNTHETIC supply: Sag bound (C_eff), Drift still unbound.
        const auto sup = runtime::syntheticFactorySupply();
        const double cEff = std::max(sup.cMinF, sup.cBaseF / (1.0 + 3.0 * k.sag));
        CHECK(std::fabs(d.effective.shelfGainDb + 6.0 * k.thermalAge) <= 1e-12 && d.effective.supportsSag && !d.effective.supportsDrift &&
                  std::fabs(d.effective.reservoirC - cEff) <= 1e-18 && d.circuit.supplyEnabled && std::fabs(d.circuit.reservoirF - cEff) <= 1e-18,
              "Age shelf / Sag bound (C_eff %.6g vs %.6g) / Drift unbound on synthetic topology", d.effective.reservoirC, cEff);
        worst = std::max(worst, peakDiff(yl, yf));
        CHECK(finite(yl), "non-finite at endpoint");
    }
    CHECK(worst <= 1e-6, "endpoint renders differ %.3e", worst);
    // Sag is audible through the factory topology's reduced SYNTHETIC supply reservoir: measured here.
    LiteState s0, s1;
    s0.knobs = {0, 0.2}; s1.knobs = {1, 0.2};
    LiteEngine l0, l1;
    CHECK(l0.prepare(s0, fs, 512, 2, e) && l1.prepare(s1, fs, 512, 2, e), "prepare");
    const Buf y0 = render(liteProc(l0), in, fs, 480, {}), y1 = render(liteProc(l1), in, fs, 480, {});
    const double sagEffect = peakDiff(y0, y1);
    auto rms = [](const Buf& y) { double s = 0; for (std::size_t i = 2400; i < y[0].size(); ++i) s += y[0][i] * y[0][i]; return std::sqrt(s / double(y[0].size() - 2400)); };
    const double sagDb = 20.0 * std::log10(rms(y1) / rms(y0));
    const double r0 = l0.engine().diagnostics().circuit.railFraction[0], r1 = l1.engine().diagnostics().circuit.railFraction[0];
    CHECK(sagEffect >= 1e-2 && sagDb < -0.5 && r1 < r0, "LITE Sag knob not audible: peak change %.3e, level %.3f dB, rail %.4f vs %.4f",
          sagEffect, sagDb, r1, r0);
    std::printf("T05 knob endpoints {0,1}^2: LITE vs full peak %.3e; effective macros exact; Sag 0->1 (SYNTHETIC supply) output change "
                "peak %.3e, level %.2f dB (50-100 ms), rail fraction at 100 ms %.4f -> %.4f\n", worst, sagEffect, sagDb, r0, r1);
}

void stateRoundTrip() {
    std::string e, t1, t2;
    LiteState s;
    s.knobs = {0.61803398874989484, 0.123456789012345678};
    s.seed = 9007199254740992ull;
    s.ui = core::JsonValue::makeObject();
    s.ui.set("scale", core::JsonValue::makeNumber(1.25));
    s.ui.set("theme", core::JsonValue::makeString("dark"));
    CHECK(saveLiteState(s, t1, e), "save %s", e.c_str());
    LiteState r;
    CHECK(loadLiteState(t1, r, e), "load %s", e.c_str());
    CHECK(saveLiteState(r, t2, e) && t1 == t2, "save/reload not deterministic");
    CHECK(r.knobs.sag == s.knobs.sag && r.knobs.thermalAge == s.knobs.thermalAge && r.seed == s.seed && r.qualityRevision == 1, "values");
    CHECK(t1.find("\"edition\":\"lite\"") != std::string::npos && t1.find("\"liteMappingVersion\":1") != std::string::npos, "keys");
    // Deterministic render from the reloaded state.
    LiteEngine a, b;
    CHECK(a.prepare(s, 48000, 512, 2, e) && b.prepare(r, 48000, 512, 2, e), "prepare");
    const Buf in = seededInput(4800, 48000, 8);
    CHECK(peakDiff(render(liteProc(a), in, 48000, 512, {}), render(liteProc(b), in, 48000, 512, {})) == 0.0, "reloaded render differs");
    // Strict rejection.
    auto rejects = [&](const std::string& from, const std::string& to) {
        std::string m = t1;
        const auto p = m.find(from);
        if (p == std::string::npos) return false;
        m.replace(p, from.size(), to);
        LiteState x;
        std::string err;
        return !loadLiteState(m, x, err) && !err.empty();
    };
    CHECK(rejects("\"edition\":\"lite\"", "\"edition\":\"full\""), "edition full accepted");
    CHECK(rejects("\"liteMappingVersion\":1", "\"liteMappingVersion\":2"), "mapping v2 accepted");
    CHECK(rejects("\"schemaVersion\":1", "\"schemaVersion\":1,\"extra\":0"), "unknown key accepted");
    CHECK(rejects("\"schemaVersion\":1", "\"schemaVersion\":1,\"schemaVersion\":1"), "duplicate key accepted");
    CHECK(rejects("\"qualityRevision\":1", "\"qualityRevision\":2"), "quality revision 2 accepted");
    CHECK(rejects("\"seed\":9007199254740992", "\"seed\":-1"), "negative seed accepted");
    CHECK(rejects("\"seed\":9007199254740992", "\"seed\":0.5"), "fractional seed accepted");
    CHECK(rejects("\"sag\":0.6", "\"sag\":1.6"), "sag > 1 accepted (must not clamp)");
    LiteState bad;
    CHECK(!loadLiteState("{\"sag\":NaN}", bad, e) && !loadLiteState("", bad, e), "malformed accepted");
    std::printf("T05 state: save/reload byte-identical (%zu bytes) and render bit-identical; 9 malformed/foreign states rejected\n", t1.size());
}

void upgradeAndCompatibility() {
    std::string e;
    LiteState s;
    s.knobs = {0.7, 0.35};
    s.seed = 42;
    state::StateDocument doc;
    CHECK(exportLiteUpgrade(s, "2026-11-04T12:00:00Z", doc, e), "export %s", e.c_str());
    const core::JsonValue* age = doc.macros.get("age");
    const core::JsonValue* drift = doc.macros.get("drift");
    CHECK(age && age->number == 0.35 && drift && drift->number == 0.25 * 0.35 && doc.macros.get("warmth")->number == 0.2 &&
          doc.macros.get("sag")->number == 0.7 && doc.seed == 42 && doc.engineModelVersion == state::kEngineModelVersion, "exact mapped values");
    std::string text;
    CHECK(state::saveState(doc, text, e), "save %s", e.c_str());
    state::StateDocument loaded;
    CHECK(state::loadState(text, loaded, e), "reload %s", e.c_str());
    LiteState back;
    LiteCompatibility c = checkLiteCompatible(loaded, back);
    CHECK(c.compatible && back.knobs.sag == 0.7 && back.knobs.thermalAge == 0.35 && back.seed == 42, "round trip into LITE: %s", c.summary().c_str());
    // Full presets needing unsupported features are rejected with a summary (never reduced).
    auto withParam = [&](const char* id, double v) {
        state::StateDocument d = loaded;
        bool found = false;
        for (auto& kv : d.baseParameters) if (kv.first == id) { kv.second = v; found = true; }
        if (!found) d.baseParameters.emplace_back(id, v);
        if (std::string(id).rfind("macro.", 0) == 0) d.macros = core::JsonValue::makeObject();
        return d;
    };
    int rejected = 0;
    for (auto [id, v] : std::vector<std::pair<const char*, double>>{{"macro.warmth", 0.5}, {"mix", 0.5}, {"input.gain", 3}, {"key.enabled", 1},
                                                                    {"macro.drift", 0.9}, {"quality.mode", 1}, {"analogNoise.enabled", 1}}) {
        LiteState x;
        const LiteCompatibility r = checkLiteCompatible(withParam(id, v), x);
        CHECK(!r.compatible && !r.issues.empty() && r.summary().find("not loadable") != std::string::npos, "%s=%g accepted by LITE", id, v);
        rejected += !r.compatible;
    }
    state::StateDocument custom = loaded;
    core::parseJson(R"({"schemaVersion":1,"id":"user.rc","name":"RC","modelRegistryVersion":"fixture-v1","nodes":["0","in","out"],
      "components":[{"id":"R1","modelId":"fixture.resistor","pins":{"p":"in","n":"out"},"parameters":{"resistanceOhm":2200}},
      {"id":"C1","modelId":"fixture.capacitor","pins":{"p":"out","n":"0"},"parameters":{"capacitanceF":1e-8}}],
      "ports":{"input":"in","output":"out","ground":"0"},"macroBindings":[]})", custom.blueprints[0], e);
    custom.faults = core::JsonValue::makeArray();
    custom.faults.array.push_back(core::JsonValue::makeObject());
    LiteState x;
    const LiteCompatibility rc = checkLiteCompatible(custom, x);
    CHECK(!rc.compatible && rc.issues.size() >= 2, "custom graph + faults accepted (%zu issues)", rc.issues.size());
    rejected += !rc.compatible;
    state::StateDocument session = loaded;
    session.kind = state::DocKind::Session;
    session.blueprints.push_back(session.blueprints[0]);
    rejected += !checkLiteCompatible(session, x).compatible;
    CHECK(rejected == 9, "rejections %d", rejected);
    // Full import also refuses what it cannot render instead of dropping it.
    SceneEngineConfig sc;
    CHECK(!sceneToEngineConfig(withParam("analogNoise.enabled", 1), 48000, 512, 2, sc, e), "full import accepted analog noise");
    CHECK(!sceneToEngineConfig(custom, 48000, 512, 2, sc, e), "full import accepted fault payload");
    std::printf("T05 upgrade/compat: export carries exact macros (age %.17g drift %.17g), round-trips into LITE; 9 full presets rejected, e.g. \"%s\"\n",
                age->number, drift->number, rc.summary().c_str());
}

void noLeakage() {
    std::string e;
    LiteState s;
    LiteEngine a, b;
    CHECK(a.prepare(s, 48000, 512, 2, e) && b.prepare(s, 48000, 512, 2, e), "prepare");
    const Buf in = seededInput(4800, 48000, 12);
    const std::vector<Ev> fullOnly = {{10, 1003, 1.0}, {20, 1007, 0.0}, {30, 1005, 1.0}, {40, 1018, 1.0}, {50, 1000, 1.0}, {60, 1010, 0.0},
                                      {70, 1002, 1.0}, {80, 1006, 0.0}};
    const Buf ya = render(liteProc(a), in, 48000, 512, fullOnly), yb = render(liteProc(b), in, 48000, 512, {});
    CHECK(peakDiff(ya, yb) == 0.0 && a.diagnostics().rejected == 8 && a.diagnostics().accepted == 0, "full-only parameter leaked into LITE");
    std::printf("T05 leakage: 8 full-only host events rejected by LITE, output bit-identical\n");
}

}  // namespace

int main() {
    {   // RtTrap positive control in this harness (explicit operator new is not elidable)
        cd::test::RtTrapScope s;
        void* volatile p = ::operator new(32);
        ::operator delete(p);
        CHECK(s.counts().allocations == 1 && s.counts().deallocations == 1, "RtTrap positive control failed");
    }
    manifestAndIdentity();
    mappingEndpoints();
    t05Render();
    knobEndpoints();
    stateRoundTrip();
    upgradeAndCompatibility();
    noLeakage();
    CHECK(g_allocs == 0, "LITE/full audio calls allocated/freed %ld times", g_allocs);
    std::printf("UX-09 RtTrap over LITE and full render calls: %ld allocations+deallocations\n", g_allocs);
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("edition_lite: all checks passed\n");
    return 0;
}
