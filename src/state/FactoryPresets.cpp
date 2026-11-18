// Factory scenes compiled into the binary. CREATIVE, SYNTHETIC starting points: names evoke a vibe
// and do not claim to emulate any specific historical hardware; no value here is a measurement (G01
// stays open). Parameter ids are looked up in the registry; an unknown id is a programming error
// (std::logic_error at first use, covered by tests). Values are validated by the tests against the
// registry ranges. Every scene runs 100% wet with Auto-Gain on (UX-02).
#include <initializer_list>
#include <stdexcept>
#include <utility>
#include "state/Preset.h"

namespace cd::state {
namespace {
using Kv = std::pair<const char*, double>;

struct Def {
    const char* id;
    const char* title;
    std::initializer_list<const char*> tags;
    std::uint64_t seed;
    std::initializer_list<Kv> values;
};

void apply(ScenePreset& p, const Kv& kv) {
    const core::ParamSpec* spec = core::findById(kv.first);
    if (!spec) throw std::logic_error(std::string("factory preset: unknown parameter ") + kv.first);
    const std::size_t ri = std::size_t(spec - core::kParams);
    for (std::size_t i = 0; i < kSlotParamCount; ++i)
        if (kSlotAIndices[i] == ri) { p.slot.params[i] = kv.second; return; }
    for (std::size_t i = 0; i < kSceneGlobalCount; ++i)
        if (kSceneGlobalIndices[i] == ri) { p.globals[i] = kv.second; return; }
    throw std::logic_error(std::string("factory preset: parameter outside the scene scope ") + kv.first);
}

std::vector<FactoryPreset> build() {
    const std::initializer_list<Def> defs = {
        {"clean-console", "Clean Console", {"factory", "clean", "reference"}, 1001,
         {{"slot.a.supply.rectifier", 2}, {"slot.a.supply.sagSensitivity", 0}, {"slot.a.condition.ageYears", 0},
          {"slot.a.condition.tolerance", 0.5}, {"slot.a.condition.capLeakage", 0}, {"slot.a.condition.resistorType", 1},
          {"slot.a.stage.drive", 0}, {"slot.a.stage.activeElement", 2}, {"slot.a.noise.humLevel", -120},
          {"slot.a.noise.thermalNoise", -120}, {"slot.a.noise.microphonics", 0}, {"slot.a.lifecycle", 0},
          {"macro.age", 0}, {"macro.sag", 0}, {"macro.drift", 0}, {"macro.warmth", 0}, {"analogNoise.enabled", 0},
          {"mix", 1}, {"gainMatch.enabled", 1}}},
        {"warm-tube-preamp", "Warm Tube Preamp", {"factory", "tube", "warm"}, 1002,
         {{"slot.a.stage.activeElement", 0}, {"slot.a.supply.rectifier", 0}, {"slot.a.stage.drive", 12},
          {"slot.a.condition.ageYears", 6}, {"macro.age", 0.15}, {"macro.sag", 0.2}, {"macro.drift", 0.1},
          {"macro.warmth", 0.55}, {"analogNoise.enabled", 0}, {"mix", 1}, {"gainMatch.enabled", 1}}},
        {"pumping-bus-glue", "Pumping Bus Glue", {"factory", "bus", "sag"}, 1003,
         {{"slot.a.supply.rectifier", 1}, {"slot.a.supply.railCapacity", 60}, {"slot.a.supply.sagSensitivity", 0.8},
          {"slot.a.stage.drive", 9}, {"macro.age", 0.1}, {"macro.sag", 0.8}, {"macro.drift", 0.1},
          {"macro.warmth", 0.35}, {"analogNoise.enabled", 0}, {"mix", 1}, {"gainMatch.enabled", 1}}},
        {"lo-fi-attic-gear", "Lo-Fi Attic Gear", {"factory", "lo-fi", "degraded"}, 1004,
         {{"slot.a.condition.ageYears", 40}, {"slot.a.condition.tolerance", 14}, {"slot.a.condition.capLeakage", 0.6},
          {"slot.a.noise.humLevel", -70}, {"slot.a.noise.thermalNoise", -75}, {"slot.a.lifecycle", 0.9},
          {"slot.a.stage.drive", 15}, {"macro.age", 0.85}, {"macro.sag", 0.65}, {"macro.drift", 0.6},
          {"macro.warmth", 0.45}, {"analogNoise.enabled", 1}, {"mix", 1}, {"gainMatch.enabled", 1}}},
        {"mint-1968-console", "Mint 1968 Console", {"factory", "console", "vintage", "mint"}, 1005,
         {{"slot.a.stage.activeElement", 2}, {"slot.a.transformer.core", 0}, {"slot.a.condition.resistorType", 0},
          {"slot.a.condition.ageYears", 1}, {"slot.a.condition.tolerance", 1.5}, {"slot.a.stage.drive", 8},
          {"slot.a.supply.rectifier", 2}, {"macro.age", 0.05}, {"macro.warmth", 0.25}, {"mix", 1}, {"gainMatch.enabled", 1}}},
        {"unserviced-studio-1978", "Unserviced Studio 1978", {"factory", "studio", "vintage", "worn"}, 1006,
         {{"slot.a.condition.ageYears", 30}, {"slot.a.condition.tolerance", 8}, {"slot.a.condition.capLeakage", 0.35},
          {"slot.a.condition.resistorType", 0}, {"slot.a.noise.humFrequency", 1}, {"slot.a.noise.humLevel", -75},
          {"slot.a.noise.thermalNoise", -85}, {"slot.a.stage.drive", 10}, {"slot.a.lifecycle", 0.5},
          {"slot.a.scenario", 1}, {"macro.age", 0.55}, {"macro.drift", 0.3}, {"macro.sag", 0.2},
          {"analogNoise.enabled", 1}, {"mix", 1}, {"gainMatch.enabled", 1}}},
        {"brownout-tube-preamp", "Brownout Tube Preamp", {"factory", "tube", "sag", "brownout"}, 1007,
         {{"slot.a.stage.activeElement", 0}, {"slot.a.supply.rectifier", 0}, {"slot.a.supply.mainsVoltage", 98},
          {"slot.a.supply.railCapacity", 40}, {"slot.a.supply.sagSensitivity", 0.9}, {"slot.a.stage.drive", 18},
          {"slot.a.stage.bias", -0.2}, {"slot.a.transformer.core", 1}, {"slot.a.condition.ageYears", 12},
          {"slot.a.condition.tolerance", 6}, {"macro.sag", 0.7}, {"macro.warmth", 0.5}, {"mix", 1}, {"gainMatch.enabled", 1}}},
        {"selenium-sag-pump", "Selenium Sag Pump", {"factory", "sag", "pump", "selenium"}, 1008,
         {{"slot.a.supply.rectifier", 1}, {"slot.a.supply.railCapacity", 25}, {"slot.a.supply.sagSensitivity", 0.85},
          {"slot.a.supply.mainsVoltage", 105}, {"slot.a.stage.activeElement", 1}, {"slot.a.stage.drive", 14},
          {"macro.sag", 0.85}, {"macro.warmth", 0.3}, {"mix", 1}, {"gainMatch.enabled", 1}}},
        {"warm-iron-transformer", "Warm Iron Transformer", {"factory", "transformer", "warm", "iron"}, 1009,
         {{"slot.a.transformer.core", 2}, {"slot.a.stage.activeElement", 2}, {"slot.a.stage.drive", 16},
          {"slot.a.stage.bias", 0.1}, {"slot.a.noise.humLevel", -100}, {"macro.warmth", 0.7},
          {"harmonics.target", -0.4}, {"harmonics.amount", 0.3}, {"mix", 1}, {"gainMatch.enabled", 1}}},
        {"jfet-glue", "JFET Glue", {"factory", "jfet", "glue"}, 1010,
         {{"slot.a.stage.activeElement", 3}, {"slot.a.stage.drive", 12}, {"slot.a.stage.bias", 0.15},
          {"slot.a.thermal.mass", 0.7}, {"macro.warmth", 0.4}, {"macro.sag", 0.35}, {"harmonics.target", 0.5},
          {"harmonics.amount", 0.25}, {"transient.preserve", 0.3}, {"mix", 1}, {"gainMatch.enabled", 1}}},
        {"bjt-grit", "BJT Grit", {"factory", "bjt", "grit", "drive"}, 1011,
         {{"slot.a.stage.activeElement", 2}, {"slot.a.stage.drive", 26}, {"slot.a.stage.bias", -0.35},
          {"slot.a.condition.tolerance", 5}, {"slot.a.condition.resistorType", 0}, {"slot.a.filter.lpfEnabled", 1},
          {"slot.a.filter.lpfFrequency", 12000}, {"harmonics.target", -0.6}, {"harmonics.amount", 0.5},
          {"mix", 1}, {"gainMatch.enabled", 1}}},
        {"thermal-bloom", "Thermal Bloom", {"factory", "thermal", "drift"}, 1012,
         {{"slot.a.thermal.mass", 0.9}, {"slot.a.thermal.driftCoefficient", 1.6}, {"slot.a.thermal.ambient", 38},
          {"slot.a.condition.ageYears", 8}, {"slot.a.stage.drive", 12}, {"slot.a.lifecycle", 0.3},
          {"macro.drift", 0.7}, {"macro.warmth", 0.45}, {"mix", 1}, {"gainMatch.enabled", 1}}},
        {"gigged-road-amp", "Gigged Road Amp", {"factory", "amp", "worn", "road"}, 1013,
         {{"slot.a.stage.activeElement", 1}, {"slot.a.condition.ageYears", 35}, {"slot.a.condition.tolerance", 12},
          {"slot.a.condition.capLeakage", 0.5}, {"slot.a.noise.microphonics", 0.6}, {"slot.a.noise.humFrequency", 1},
          {"slot.a.noise.humLevel", -68}, {"slot.a.noise.thermalNoise", -80}, {"slot.a.stage.drive", 22},
          {"slot.a.lifecycle", 0.66}, {"slot.a.scenario", 1}, {"slot.a.filter.lpfEnabled", 1},
          {"slot.a.filter.lpfFrequency", 7500}, {"macro.age", 0.7}, {"analogNoise.enabled", 1}, {"mix", 1}, {"gainMatch.enabled", 1}}},
        {"hum-and-hiss-lo-fi", "Hum & Hiss Lo-Fi", {"factory", "lo-fi", "noise", "hum"}, 1014,
         {{"slot.a.condition.ageYears", 45}, {"slot.a.condition.capLeakage", 0.7}, {"slot.a.condition.tolerance", 15},
          {"slot.a.noise.humFrequency", 0}, {"slot.a.noise.humLevel", -55}, {"slot.a.noise.thermalNoise", -62},
          {"slot.a.filter.lpfEnabled", 1}, {"slot.a.filter.lpfFrequency", 5500}, {"slot.a.stage.drive", 14},
          {"analogNoise.enabled", 1}, {"macro.age", 0.5}, {"mix", 1}, {"gainMatch.enabled", 1}}},
        {"sidechain-pump-ready", "Sidechain Pump Ready", {"factory", "sidechain", "pump", "sag"}, 1015,
         {{"slot.a.supply.rectifier", 1}, {"slot.a.supply.railCapacity", 30}, {"slot.a.supply.sagSensitivity", 0.9},
          {"slot.a.stage.drive", 10}, {"macro.sag", 0.6}, {"sidechain.source", 1}, {"sidechain.depth", 0.8},
          {"sidechain.hpf", 80}, {"transient.preserve", 0.5}, {"mix", 1}, {"gainMatch.enabled", 1}}},
    };
    std::vector<FactoryPreset> out;
    for (const Def& d : defs) {
        FactoryPreset f;
        f.id = d.id;
        f.scene.meta.title = d.title;
        f.scene.meta.author = "CircuitDrift Factory";
        f.scene.meta.tags.assign(d.tags.begin(), d.tags.end());
        f.scene.slot.blueprintId = std::string("factory.") + d.id;
        f.scene.slot.seed = d.seed;
        for (const Kv& kv : d.values) apply(f.scene, kv);
        out.push_back(std::move(f));
    }
    return out;
}
}  // namespace

const std::vector<FactoryPreset>& factoryPresets() {
    static const std::vector<FactoryPreset> presets = build();
    return presets;
}

}  // namespace cd::state
