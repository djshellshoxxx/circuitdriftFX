// CircuitDrift blueprint: compiler (03-blueprints spec "Compiler checks in order" 1-6).
// Turns a parsed Blueprint into an immutable PreparedGraph (generation + cost report) using the
// DeviceManifest model registry. Unsupported models are rejected; pins are never guessed; user
// wiring is never silently repaired. Result is "Validated for this configuration" only (Fs, OS),
// on SYNTHETIC fixture models; no physical validation (G01 open).
// The device/source catalogue here is a labelled synthetic fixture registry (fixture-v1).
// Threading: compile() and GraphSlot are worker-thread only (allocate, may take seconds).
// A PreparedGraph is immutable after compile and may be read from any thread; the audio thread
// receives a PreparedHandle but never owns its deallocation (retire via the worker).
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include "blueprint/BlueprintGraph.h"
#include "dsp/DeviceManifest.h"
#include "dsp/MnaSolver.h"

namespace cd::blueprint {

// Ideal DC/arbitrary voltage source model (synthetic fixture; node p is +). Not a device model.
struct SourceModel {
    std::string modelId;
    int revision = 1;
    std::string paramName = "voltageV";
    double min = -100, max = 100;
};

class ModelRegistry {
public:
    std::string version;
    std::vector<dsp::DeviceManifest> devices;
    std::vector<SourceModel> sources;
    const dsp::DeviceManifest* findDevice(const std::string& modelId) const noexcept;
    const SourceModel* findSource(const std::string& modelId) const noexcept;
    // SYNTHETIC registry "fixture-v1": fixture.resistor/capacitor/diode + fixture.vsource.
    static ModelRegistry syntheticFixtures();
};

struct RuntimeConfig {
    double sampleRate = 48000;      // host Fs, 8 kHz..384 kHz
    int oversampling = 1;           // 1,2,4,8,16: electrical rate = Fs * OS
    double fullScaleVolts = 1.0;    // voltage of 0 dBFS at the input terminal, (0,100]
};

struct CostReport {
    int components = 0, nodes = 0, pinConnections = 0, thermalNodes = 0, nonlinear = 0;
    int voltageSources = 0, unknowns = 0, dcIterations = 0;
    double probePeakVolts[3] = {0, 0, 0};   // silence, -18 dBFS sine, -6 dBFS transient
    int probeMaxNewton = 0;
    double estimatedFlopsPerSample = 0;     // dense LU cost estimate per electrical sample, informational
    std::size_t estimatedBytes = 0;
    double sampleRate = 0;
    int oversampling = 1;
    bool validatedForThisConfigurationOnly = false;
};

struct ParamTarget {                         // resolved "component.field" reference
    bool bound = false;
    int component = -1;                      // index into Blueprint::components
    int element = -1;                        // netlist element index
    int paramIndex = -1;                     // index into the netlist element's param[] (-1 for source value)
};

using Generation = std::uint64_t;

class PreparedGraph {
public:
    PreparedGraph() = default;
    PreparedGraph(const PreparedGraph&) = delete;
    PreparedGraph& operator=(const PreparedGraph&) = delete;
    Generation generation = 0;               // assigned by the compile caller (GraphSlot)
    Blueprint blueprint;
    dsp::Netlist netlist;                    // elements point into models_ (stable, never copied)
    int inputElement = -1;                   // injected drive source in netlist.elements
    int outputNode = 0;                      // netlist node index of ports.output
    std::vector<int> elementOfComponent;     // blueprint component -> netlist element
    std::vector<std::string> nodeNames;      // netlist node index -> blueprint node id (0 = "0")
    std::array<ParamTarget, kModSlotCount> modSlots;
    std::vector<ParamTarget> macroTargets;   // parallel to blueprint.macroBindings
    CostReport cost;
    RuntimeConfig config;
    // Fresh solver at the prepared DC operating point (worker thread; allocates).
    bool makeSolver(dsp::MnaSolver& out, std::string& error) const;
    std::vector<dsp::DeviceManifest> models_;  // copies of the manifests the netlist uses
};
using PreparedHandle = std::shared_ptr<const PreparedGraph>;

struct CompileResult {
    bool ok = false;
    PreparedHandle graph;                    // null unless ok
    Diagnostics diagnostics;
    CostReport cost;                         // filled as far as compilation got
    int failedCheck = 0;                     // 1..6, 0 when ok
};

// Worker only; bounded input already parsed. `generation` is stored in the graph.
CompileResult compile(const Blueprint&, const ModelRegistry&, const RuntimeConfig&, Generation generation = 0);
// Parse + compile.
CompileResult compileText(std::string_view json, const ModelRegistry&, const RuntimeConfig&, Generation generation = 0);

// Components whose capacitor charge/magnetic/thermal state may carry from `from` to `to`:
// same component ID, model ID, model revision and identical pin->node wiring. Pairs of indices.
std::vector<std::pair<int, int>> stableStateMap(const PreparedGraph& from, const PreparedGraph& to);

// Holds the active graph. A failed compile leaves the active graph, and its generation, untouched;
// a successful one is a single generation increment.
class GraphSlot {
public:
    CompileResult compileAndApply(std::string_view json, const ModelRegistry&, const RuntimeConfig&);
    CompileResult compileAndApply(const Blueprint&, const ModelRegistry&, const RuntimeConfig&);
    PreparedHandle active() const;
    Generation generation() const;
private:
    std::mutex applyMu_;            // serialises compile+apply (worker)
    mutable std::mutex mu_;
    PreparedHandle active_;
    Generation gen_ = 0;
};

}  // namespace cd::blueprint
