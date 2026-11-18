// CircuitDrift plugin UI: per-component readouts for labels, overlays and the inspector (CD-F17).
// Everything shown is either telemetry, EffectiveTargets, a parameter value or the static component
// table; anything the engine does not publish is reported as "not exposed", never invented.
#pragma once
#include <vector>
#include "ui/EditorBridge.h"
#include "ui/workbench/PcbModel.h"

namespace cd::plugin::ui {

struct ValueRow { juce::String label, value; };

struct PartReadout {
    bool hasTemp = false;
    double tempC = 0, powerMw = 0;
    pcb::ThermalState state = pcb::ThermalState::Normal;
    std::vector<ValueRow> values;   // nominal / base / effective with units
    std::vector<ValueRow> live;     // temperature, power, terminal voltages/currents
    // Voltage overlay: only nodes the telemetry actually measures carry a value.
    bool hasVoltage = false;
    double voltage = 0, voltageScale = 1;
    juce::String voltageText, voltageNote;
    bool fault = false;
    juce::String condition;         // "Nominal (no fault applied)" or the fault description
};

struct ReadoutContext {
    EditorBridge* bridge = nullptr;
    const engine::TelemetryFrame* frame = nullptr;
    bool frameValid = false;
    int slot = 0, channel = 0;
    engine::EffectiveTargets eff{};
    std::vector<FaultInfo> faults;
};

PartReadout readPart(int componentId, const ReadoutContext& ctx);

// "470 uF", "1.2 mA", "288.1 V": SI prefix chosen for the magnitude (ASCII-safe "u").
juce::String formatSi(double value, const char* unit, int significant = 3);
const char* materialName(int resistorType) noexcept;     // Carbon Comp / Metal Film / Wirewound
const char* familyName(int family) noexcept;
const char* rectifierName(int rectifier) noexcept;
const char* coreName(int core) noexcept;
const char* faultName(FaultType t) noexcept;

}  // namespace cd::plugin::ui
