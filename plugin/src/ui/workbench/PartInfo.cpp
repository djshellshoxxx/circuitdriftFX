#include "ui/workbench/PartInfo.h"

#include <cmath>
#include "ui/workbench/ParamOps.h"

namespace cd::plugin::ui {
namespace {
using engine::ComponentId;

juce::String degC() { return juce::String(juce::CharPointer_UTF8("\xC2\xB0")) + "C"; }
juce::String mu() { return juce::String(juce::CharPointer_UTF8("\xC2\xB5")); }

juce::String fixed(double v, int d) { return juce::String(v, d); }
juce::String sgn(double v, int d) { return (v >= 0 ? "+" : "") + juce::String(v, d); }

ValueRow row(const char* l, const juce::String& v) { return {l, v}; }
}  // namespace

juce::String formatSi(double v, const char* unit, int significant) {
    const double a = std::abs(v);
    struct P { double s; const char* p; };
    const P ps[] = {{1e9, "G"}, {1e6, "M"}, {1e3, "k"}, {1.0, ""}, {1e-3, "m"}, {1e-6, nullptr}, {1e-9, "n"}, {1e-12, "p"}};
    double scale = 1.0;
    juce::String prefix;
    if (a == 0) { return "0 " + juce::String(unit); }
    for (const auto& p : ps)
        if (a >= p.s * 0.9995) { scale = p.s; prefix = p.p ? juce::String(p.p) : mu(); break; }
    if (prefix.isEmpty() && scale == 1.0 && a < 1e-12) { scale = 1e-12; prefix = "p"; }
    const double x = v / scale;
    const int decimals = std::abs(x) >= 100 ? 0 : std::abs(x) >= 10 ? 1 : 2;
    (void) significant;
    return juce::String(x, decimals) + " " + prefix + unit;
}

const char* materialName(int t) noexcept {
    static const char* n[] = {"Carbon Comp", "Metal Film", "Wirewound"};
    return t >= 0 && t < 3 ? n[t] : "?";
}
const char* familyName(int f) noexcept {
    static const char* n[] = {"12AX7 triode", "EF86 pentode", "BJT", "JFET"};
    return f >= 0 && f < 4 ? n[f] : "?";
}
const char* rectifierName(int r) noexcept {
    static const char* n[] = {"5U4G tube", "Selenium", "Silicon diode"};
    return r >= 0 && r < 3 ? n[r] : "?";
}
const char* coreName(int c) noexcept {
    static const char* n[] = {"Nickel", "Steel", "Iron"};
    return c >= 0 && c < 3 ? n[c] : "?";
}
const char* faultName(FaultType t) noexcept {
    switch (t) {
        case FaultType::None: return "none";
        case FaultType::ElectrolyticOpen: return "Electrolytic open";
        case FaultType::ElectrolyticLeaky: return "Electrolytic leaky";
        case FaultType::CrackedResistor: return "Cracked resistor";
        case FaultType::MicrophonicTube: return "Microphonic tube";
    }
    return "";
}

PartReadout readPart(int id, const ReadoutContext& c) {
    PartReadout r;
    const auto& part = pcb::parts()[static_cast<std::size_t>(id)];
    const auto cid = static_cast<ComponentId>(id);
    const int ch = std::clamp(c.channel, 0, engine::kMaxChannels - 1), slot = std::clamp(c.slot, 0, engine::kSlots - 1);
    const bool live = c.frame != nullptr && c.frameValid;
    const auto& eff = c.eff;
    ParamOps ops(*c.bridge);

    // ---- Condition / fault ----
    r.condition = "Nominal (no fault applied)";
    for (const auto& f : c.faults)
        if (f.enabled && f.type != FaultType::None && f.componentKey == part.key) {
            r.fault = true;
            r.condition = juce::String(faultName(f.type)) + ", severity " + fixed(f.severity, 2) + (f.intermittent ? ", intermittent" : "");
            break;
        }

    // ---- Live telemetry ----
    if (live) {
        const auto& st = c.frame->slot[slot];
        r.hasTemp = true;
        r.tempC = st.temperatureK[ch][id] - 273.15;
        r.powerMw = st.powerW[ch][id] * 1000.0;
        r.state = pcb::thermalState(part.ref, r.tempC);
        r.live.push_back(row("Temperature", fixed(r.tempC, 1) + " " + degC() + "  [" + pcb::thermalStateName(r.state) + "]"));
        r.live.push_back(row("Reference", juce::String("cool < ") + fixed(part.ref.coolBelow, 0) + ", normal <= " + fixed(part.ref.normalMax, 0) +
                                              ", stress >= " + fixed(part.ref.stress, 0) + " " + degC()));
        r.live.push_back(row("Dissipation", formatSi(st.powerW[ch][id], "W")));
        const double vnom = st.railNominalVolts;
        switch (cid) {
            case ComponentId::V1:
                r.live.push_back(row("Stage current", formatSi(st.stageCurrentA[ch], "A")));
                break;
            case ComponentId::C_res:
                r.hasVoltage = true; r.voltage = st.railVolts; r.voltageScale = vnom > 0 ? vnom : 300.0;
                r.voltageText = sgn(st.railVolts, 0) + " V"; r.voltageNote = "V_res";
                r.live.push_back(row("V_res (reservoir)", sgn(st.railVolts, 1) + " V"));
                r.live.push_back(row("DC rail", sgn(st.railDcVolts, 1) + " V, nominal " + fixed(vnom, 0) + " V"));
                if (vnom > 0) r.live.push_back(row("Droop", fixed(100.0 * (1.0 - st.railVolts / vnom), 1) + " % below nominal"));
                break;
            case ComponentId::C_filt: case ComponentId::C_dec: case ComponentId::R_load:
                r.hasVoltage = true; r.voltage = st.railDcVolts; r.voltageScale = vnom > 0 ? vnom : 300.0;
                r.voltageText = sgn(st.railDcVolts, 0) + " V"; r.voltageNote = "DC rail";
                r.live.push_back(row("DC rail node", sgn(st.railDcVolts, 1) + " V (rail telemetry; node drop not exposed)"));
                break;
            case ComponentId::C_in:
                r.hasVoltage = true; r.voltage = c.frame->inPeak[ch]; r.voltageScale = 1.0;
                r.voltageText = "pk " + fixed(c.frame->inPeak[ch], 2) + " FS"; r.voltageNote = "audio in peak";
                r.live.push_back(row("Input level", "peak " + fixed(c.frame->inPeak[ch], 3) + " FS, RMS " + fixed(c.frame->inRms[ch], 3) + " FS"));
                break;
            case ComponentId::C_out:
                r.hasVoltage = true; r.voltage = c.frame->outPeak[ch]; r.voltageScale = 1.0;
                r.voltageText = "pk " + fixed(c.frame->outPeak[ch], 2) + " FS"; r.voltageNote = "audio out peak";
                r.live.push_back(row("Output level", "peak " + fixed(c.frame->outPeak[ch], 3) + " FS, RMS " + fixed(c.frame->outRms[ch], 3) + " FS"));
                break;
            case ComponentId::T_out:
                if (std::abs(st.fluxDensityT[ch]) > 0 || std::abs(st.fieldH[ch]) > 0)
                    r.live.push_back(row("Magnetic state", "B " + fixed(st.fluxDensityT[ch], 3) + " T, H " + fixed(st.fieldH[ch], 1) + " A/m, flux " + formatSi(st.fluxWb[ch], "Wb")));
                else
                    r.live.push_back(row("Magnetic state", "not exposed by this model"));
                break;
            default: break;
        }
        if (!r.hasVoltage) { r.voltageNote = "no voltage telemetry for this node"; }
    }

    // ---- Nominal / base / effective ----
    const bool effValid = eff.reservoirCapF > 0 || eff.biasVolts != 0 || eff.ageYears != 0 || eff.saturationFluxWb > 0 || eff.mainsVolts != 117;
    const juce::String dash = juce::String(juce::CharPointer_UTF8("\xE2\x80\x94"));
    switch (cid) {
        case ComponentId::C_res: {
            const double base = ops.real("supply.railCapacity") * 1e-6;
            r.values.push_back(row("Nominal (factory)", "100 " + mu() + "F"));
            r.values.push_back(row("Base (setting)", formatSi(base, "F")));
            r.values.push_back(row("Effective", eff.reservoirCapF > 0 ? formatSi(eff.reservoirCapF, "F") + " (Age/Sag/condition applied)" : dash + " (targets not published)"));
            if (eff.reservoirCapBaseF > 0) r.values.push_back(row("Base after macros", formatSi(eff.reservoirCapBaseF, "F")));
            break;
        }
        case ComponentId::V1: {
            r.values.push_back(row("Nominal bias", "0.00 (centred)"));
            r.values.push_back(row("Base (setting)", fixed(ops.real("stage.bias"), 2) + " normalized"));
            r.values.push_back(row("Effective", effValid ? fixed(eff.biasNormalized, 2) + " norm = " + sgn(eff.biasVolts, 2) + " V grid bias" : dash));
            if (live) r.values.push_back(row("Element", familyName(c.frame->slot[slot].family)));
            r.values.push_back(row("Drive", effValid ? sgn(eff.driveDb, 1) + " dB" : dash));
            break;
        }
        case ComponentId::R_g: case ComponentId::R_load: case ComponentId::R_k: case ComponentId::R_dec: case ComponentId::R_g2: case ComponentId::R_filt: {
            const int type = static_cast<int>(std::lround(ops.real("condition.resistorType")));
            r.values.push_back(row("Nominal value", "not exposed by the engine"));
            r.values.push_back(row("Material (setting)", materialName(type)));
            r.values.push_back(row("Age (base)", fixed(ops.real("condition.ageYears"), 1) + " years"));
            r.values.push_back(row("Age (effective)", effValid ? fixed(eff.ageYears, 1) + " years" : dash));
            r.values.push_back(row("Tolerance", "+-" + fixed(ops.real("condition.tolerance"), 1) + " % (base), sigma " + (effValid ? fixed(eff.toleranceSigma, 2) : dash)));
            break;
        }
        case ComponentId::C_in: case ComponentId::C_k: case ComponentId::C_out: case ComponentId::C_dec: case ComponentId::C_g2: case ComponentId::C_filt:
            r.values.push_back(row("Nominal value", "not exposed by the engine"));
            r.values.push_back(row("Leakage (base)", fixed(ops.real("condition.capLeakage"), 2) + " of span"));
            r.values.push_back(row("Leakage (effective)", effValid ? fixed(eff.leakageFraction, 2) + " of span" : dash));
            break;
        case ComponentId::T_out:
            r.values.push_back(row("Core", effValid ? coreName(eff.core) : dash));
            r.values.push_back(row("Saturation flux", eff.saturationFluxWb > 0 ? formatSi(eff.saturationFluxWb, "Wb") : dash));
            r.values.push_back(row("Flux drive factor", effValid ? fixed(eff.fluxDriveFactor, 2) : dash));
            break;
        case ComponentId::D_rect:
            r.values.push_back(row("Rectifier", effValid ? rectifierName(eff.rectifier) : dash));
            r.values.push_back(row("Mains (effective)", effValid ? fixed(eff.mainsVolts, 0) + " V AC, " + fixed(eff.mainsHz, 0) + " Hz (separate from the DC rail)" : dash));
            break;
        case ComponentId::Chassis:
            r.values.push_back(row("Ambient", effValid ? fixed(eff.ambientK - 273.15, 1) + " " + degC() : dash));
            r.values.push_back(row("Thermal mass scale", effValid ? fixed(eff.thermalMassScale, 2) : dash));
            break;
        default: break;
    }
    return r;
}

}  // namespace cd::plugin::ui
