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

// Appended families (P03 engine catalogue) keep the original enumerator values stable.
//  Electrolytic: series ESR + (C || leakage) with internal capacitor-voltage state.
//  SaturatingInductor: flux-linkage state, i(lambda) = lambda/L * (1 + |lambda/lambda_s|^p).
//  KorenTriode / KorenPentode: Koren-form vacuum tube equations (grid, plate, cathode[, screen]).
//  EbersMollNpn: transport-form Ebers-Moll NPN (base, collector, emitter).
//  SquareLawNjfet: Shichman-Hodges N-JFET with softplus cutoff (gate, drain, source).
enum class DeviceEquation : unsigned char {
    Resistor, Capacitor, ShockleyDiode,
    Electrolytic, SaturatingInductor, KorenTriode, KorenPentode, EbersMollNpn, SquareLawNjfet
};
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
    double maxNormalizedFlux = 20;       // SaturatingInductor: |lambda/lambda_s| cap, C1 linear beyond
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

// SYNTHETIC CIRCUIT CATALOGUE (engine factory graph, P03): electrolytic/film capacitor, output
// transformer magnetizing branch, 12AX7-like triode, EF86-like pentode, small-signal NPN and N-JFET.
// Every entry is labelled "synthetic-fixture, not calibrated hardware (G01)"; numbers are round
// illustrative values in the range of textbook equation fits, not measurements of any device.
const std::vector<DeviceManifest>& syntheticCircuitCatalogue();
const char* syntheticCircuitJson(int index) noexcept;
constexpr int kSyntheticCircuitCount = 7;
// Searches the fixture catalogue, then the circuit catalogue.
const DeviceManifest* findSyntheticDevice(const std::string& modelId);

}  // namespace cd::dsp
