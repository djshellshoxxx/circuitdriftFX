// CircuitDrift dsp: device model manifests (P02, 01-runtime "coupled simulation contract").
// A manifest names a built-in equation family (q, f and analytic derivatives live in MnaSolver),
// its pins, parameter units/bounds, solver scales, domain bounds and calibration provenance.
// No component name alone constitutes a model. Only labelled synthetic fixtures ship here;
// real devices remain blocked on G01.
// Threading: parse/catalogue are non-audio (allocate); manifests are immutable once parsed.
// Additive (resume packet 2, temperature feedback): an optional "temperature" object declares a
// LINEAR temperature law for one parameter, p(T) = p_nom * (1 + k * alpha * (T - T_ref)), with T
// clamped to the declared [minK, maxK] model domain and the result clamped to the parameter's
// manifest bounds (both clamps reported). k is the runtime sensitivity multiplier (Drift macro:
// 1 + 4d; 1 = the manifest's declared law). Only synthetic fixtures carry a law (G01 open): the
// coefficient is an arbitrary round number, not a measured tempco.
#pragma once
#include <string>
#include <vector>
#include "core/Json.h"

namespace cd::dsp {

enum class DeviceEquation : unsigned char { Resistor, Capacitor, ShockleyDiode };
enum class ProvenanceKind : unsigned char { SyntheticFixture, Measured };

struct ManifestParam {
    std::string name, unit;
    double min = 0, max = 0, def = 0;
};

struct TemperatureLaw {
    bool present = false;
    int paramIndex = -1;                 // index into params
    double tempcoPerK = 0;               // alpha [1/K], finite, |alpha| <= 0.1
    double referenceK = 298.15;          // T_ref [K]
    double minK = 0, maxK = 0;           // model temperature domain [K], 0 < minK <= T_ref <= maxK
    // Evaluates the law (noexcept, allocation-free). clamped is set when T, k or the result was
    // clamped. Returns `nominal` unchanged when no law is present.
    double evaluate(double nominal, double temperatureK, double sensitivity, double paramMin, double paramMax,
                    bool& clamped) const noexcept;
};

struct DeviceManifest {
    std::string modelId;
    int revision = 0;
    DeviceEquation equation = DeviceEquation::Resistor;
    std::vector<std::string> pins;
    std::vector<ManifestParam> params;   // order fixed by the equation family (see .cpp)
    double voltageScaleV = 1, currentScaleA = 1;
    double maxExpArg = 40;               // exponent cap; beyond it the model is C1 linear-extended
    ProvenanceKind provenance = ProvenanceKind::SyntheticFixture;
    std::string provenanceNote, provenanceSource;
    TemperatureLaw temperature{};        // optional (absent for the original fixtures)
    int paramIndex(const std::string& name) const noexcept;
};

// Validates against docs/planned-features/schemas/device-manifest.schema.json semantics plus
// family rules (pin count, required parameter names/units, bounds, positive scales).
bool parseDeviceManifest(const core::JsonValue& v, DeviceManifest& out, std::string& error);

// SYNTHETIC FIXTURES ONLY: fixture.resistor, fixture.capacitor, fixture.diode. Not real devices.
const std::vector<DeviceManifest>& syntheticFixtureCatalogue();
const DeviceManifest* findSyntheticFixture(const std::string& modelId);
// Embedded manifest JSON text for the fixtures (also used by tests for mutation cases).
const char* syntheticFixtureJson(int index) noexcept;
constexpr int kSyntheticFixtureCount = 3;
// SYNTHETIC thermal fixtures (additive; NOT in syntheticFixtureCatalogue(), whose size and the
// blueprint fixture-v1 registry stay unchanged): fixture.resistor.tempco = ideal resistor with an
// arbitrary linear tempco of +3.9e-3 /K about 298.15 K, domain 233.15..473.15 K.
// findSyntheticFixture() also resolves these ids.
const std::vector<DeviceManifest>& syntheticThermalFixtures();
const char* syntheticThermalFixtureJson(int index) noexcept;
constexpr int kSyntheticThermalFixtureCount = 1;

}  // namespace cd::dsp
