// CircuitDrift edition: manifests, identities, LITE mapping v1 (see EditionManifest.h). Owner: cd_edition.
#include "edition/EditionManifest.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include "blueprint/Compiler.h"
#include "runtime/ProcessorGraph.h"

namespace cd::edition {
namespace {

// PROPOSED PLACEHOLDERS (G08/G09). ".invalid" is reserved (RFC 2606) so none of these can collide
// with a real registered identifier or be mistaken for one.
constexpr EditionIdentity kFull{Edition::Full, "CircuitDrift",
    {{{PluginFormat::Vst3, "PLACEHOLDER-G09-VST3-CLASSID-CIRCUITDRIFT-FULL", "invalid.placeholder.circuitdrift.full.vst3"},
      {PluginFormat::Au, "PLACEHOLDER-G09-AU-SUBTYPE-CIRCUITDRIFT-FULL", "invalid.placeholder.circuitdrift.full.component"},
      {PluginFormat::Aax, "PLACEHOLDER-G08-AAX-PRODUCTID-CIRCUITDRIFT-FULL", "invalid.placeholder.circuitdrift.full.aaxplugin"},
      {PluginFormat::Clap, "invalid.placeholder.circuitdrift.full", "invalid.placeholder.circuitdrift.full.clap"}}}};
constexpr EditionIdentity kLite{Edition::Lite, "CircuitDrift LITE",
    {{{PluginFormat::Vst3, "PLACEHOLDER-G09-VST3-CLASSID-CIRCUITDRIFT-LITE", "invalid.placeholder.circuitdrift.lite.vst3"},
      {PluginFormat::Au, "PLACEHOLDER-G09-AU-SUBTYPE-CIRCUITDRIFT-LITE", "invalid.placeholder.circuitdrift.lite.component"},
      {PluginFormat::Aax, "PLACEHOLDER-G08-AAX-PRODUCTID-CIRCUITDRIFT-LITE", "invalid.placeholder.circuitdrift.lite.aaxplugin"},
      {PluginFormat::Clap, "invalid.placeholder.circuitdrift.lite", "invalid.placeholder.circuitdrift.lite.clap"}}}};

constexpr std::uint32_t kLitePrimary[] = {kLiteSagHostId, kLiteThermalAgeHostId};
constexpr std::uint32_t kFullPrimary[] = {1000, 1001, 1002, 1003};

constexpr EditionManifest kFullManifest{Edition::Full, "CircuitDrift (full)", std::span<const std::uint32_t>(kFullPrimary),
                                        true, true, true, true, true, true, true, 0};
constexpr EditionManifest kLiteManifest{Edition::Lite, "CircuitDrift LITE", std::span<const std::uint32_t>(kLitePrimary),
                                        false, false, false, false, false, false, false, kLiteMappingVersion};

double knob(double v, double def) noexcept { return std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : def; }

// SYNTHETIC fixture-v1 clean factory topology (same element values as runtime::syntheticDiodeClipper).
constexpr const char* kFactoryBlueprint =
    R"BP({"schemaVersion":1,"id":"lite.factory.clean","name":"LITE clean factory topology (SYNTHETIC fixture)",)BP"
    R"BP("modelRegistryVersion":"fixture-v1","nodes":["0","in","out"],"components":[)BP"
    R"BP({"id":"R1","modelId":"fixture.resistor","pins":{"p":"in","n":"out"},"parameters":{"resistanceOhm":1000}},)BP"
    R"BP({"id":"D1","modelId":"fixture.diode","pins":{"anode":"out","cathode":"0"},)BP"
    R"BP("parameters":{"saturationCurrentA":1e-14,"emissionCoefficient":1,"thermalVoltageV":0.025852}},)BP"
    R"BP({"id":"D2","modelId":"fixture.diode","pins":{"anode":"0","cathode":"out"},)BP"
    R"BP("parameters":{"saturationCurrentA":1e-14,"emissionCoefficient":1,"thermalVoltageV":0.025852}},)BP"
    R"BP({"id":"C1","modelId":"fixture.capacitor","pins":{"p":"out","n":"0"},"parameters":{"capacitanceF":1e-8}}],)BP"
    R"BP("ports":{"input":"in","output":"out","ground":"0"},"macroBindings":[]})BP";

}  // namespace

const EditionIdentity& editionIdentity(Edition e) noexcept { return e == Edition::Lite ? kLite : kFull; }

bool identitiesDistinct(std::string& why) {
    std::set<std::string> seen;
    for (const EditionIdentity* id : {&kFull, &kLite})
        for (const FormatIdentity& f : id->formats)
            for (const char* s : {f.pluginId, f.bundleId}) {
                if (!s || !*s) { why = "empty identifier"; return false; }
                if (!seen.insert(s).second) { why = std::string("duplicate identifier ") + s; return false; }
            }
    why.clear();
    return true;
}

const EditionManifest& editionManifest(Edition e) noexcept { return e == Edition::Lite ? kLiteManifest : kFullManifest; }

bool exposes(Edition e, std::uint32_t hostId) noexcept {
    if (!core::findByHostId(hostId)) return false;
    const bool liteOnly = hostId == kLiteSagHostId || hostId == kLiteThermalAgeHostId;
    return e == Edition::Lite ? liteOnly : !liteOnly;
}

LiteMapped mapLite(const LiteKnobs& k) noexcept {
    const double s = knob(k.sag, kLiteDefaultSag), a = knob(k.thermalAge, kLiteDefaultThermalAge);
    LiteMapped m;
    m.macros.sag = s;
    m.macros.age = a;
    m.macros.drift = kLiteDriftPerAge * a;
    m.macros.warmth = kLiteWarmth;
    m.output.inputGainDb = 0; m.output.outputTrimDb = 0; m.output.mix = 1; m.output.autoGain = true;
    m.output.hpfEnabled = true; m.output.hpfCutoffHz = kLiteHpfHz; m.output.softClipEnabled = true; m.output.hostBypass = false;
    m.quality.mode = runtime::QualityMode::Eco;
    m.quality.family = dsp::OsFamily::Fir;
    m.quality.zeroLatency = false;
    m.analogNoise = false;
    m.pitchDelay = false;
    return m;
}

const char* liteFactoryBlueprintJson() noexcept { return kFactoryBlueprint; }

bool circuitFromBlueprint(std::string_view json, double fs, int maxBlock, int channels, const runtime::QualitySettings& quality,
                          PreparedEngineConfig& cfg, std::string& err) {
    dsp::Oversampler::Spec spec;
    runtime::QualityInfo info;
    if (!runtime::Engine::resolveQuality(quality, 0, spec, info)) { err = "invalid quality settings"; return false; }
    const auto r = blueprint::compileText(json, blueprint::ModelRegistry::syntheticFixtures(),
                                          blueprint::RuntimeConfig{fs, spec.factor, 1.0}, 1);
    if (!r.ok || !r.graph) {
        err = "blueprint rejected by compiler";
        if (!r.diagnostics.items.empty()) err += ": " + r.diagnostics.items.front().message;
        return false;
    }
    runtime::GraphTarget t;
    t.sampleRate = fs; t.maxBlock = maxBlock; t.channels = channels; t.os = spec;
    t.maxNewton = info.maxNewton; t.padToFamilyMax = !quality.zeroLatency;
    runtime::CircuitConfig cc;
    if (!runtime::circuitConfigFromGraph(*r.graph, t, cc, err)) return false;
    cfg.graph = r.graph;
    cfg.engine.circuit = std::move(cc);
    cfg.engine.sampleRate = fs; cfg.engine.maxBlock = maxBlock; cfg.engine.channels = channels;
    cfg.engine.quality = quality;
    return true;
}

bool liteEngineConfig(const LiteKnobs& k, double fs, int maxBlock, int channels, PreparedEngineConfig& out, std::string& err) {
    const LiteMapped m = mapLite(k);
    PreparedEngineConfig c;
    if (!circuitFromBlueprint(kFactoryBlueprint, fs, maxBlock, channels, m.quality, c, err)) return false;
    c.engine.initial = m.output;
    c.engine.initialMacros = m.macros;
    c.engine.macroBindings = runtime::syntheticMacroBindings();
    c.engine.slotRole = core::ParamScope::SlotA;
    out = std::move(c);
    return true;
}

}  // namespace cd::edition
