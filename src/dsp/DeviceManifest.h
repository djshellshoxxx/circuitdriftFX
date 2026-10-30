// CircuitDrift dsp: device model manifests (P02, 01-runtime "coupled simulation contract").
// A manifest names a built-in equation family (q, f and analytic derivatives live in MnaSolver),
// its pins, parameter units/bounds, solver scales, domain bounds and calibration provenance.
// No component name alone constitutes a model. Only labelled synthetic fixtures ship here;
// real devices remain blocked on G01.
// Threading: parse/catalogue are non-audio (allocate); manifests are immutable once parsed.
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

}  // namespace cd::dsp
