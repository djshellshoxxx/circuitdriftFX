// CircuitDrift uimodel: tooltip catalogue + timer. Owner: cd_uimodel. Message thread only.
#include "uimodel/Tooltip.h"
#include <cctype>

namespace cd::uimodel {

namespace {
const char* kProv = "Synthetic fixture (not a measured or named device; G01 calibration gate open). ";
}

const std::vector<TooltipEntry>& tooltipCatalogue() {
    static const std::vector<TooltipEntry> cat = {
        {"fixture.resistor", 1,
         "Linear resistance element; its circuit role (series, shunt, pull-up) comes from where it is wired.",
         "Not modelled: ideal linear fixture with no composition or excess-noise model.",
         "resistanceOhm [Ohm]",
         "Synthetic fixture (not a measured or named device; G01 calibration gate open). Ideal linear resistor for solver validation.",
         "Ideal linear resistor used to validate the circuit solver. Its role in the circuit depends on its wiring. It is a synthetic fixture, not a measured component.",
         "This tooltip describes a synthetic fixture resistor. It follows Ohm's law exactly: current is voltage divided by resistance. "
         "A pull-up is a circuit role of a resistor, not a separate material. The fixture does not model Johnson noise, excess noise or "
         "composition differences such as carbon composition, metal film or wirewound, so swapping it is not promised to change the sound audibly. "
         "Its resistance is bounded by the manifest and the solver uses fixed numeric scales. Real device models remain blocked on measured data (G01).",
         "What changes in audio: changing the resistance moves voltage division and filter corners in the surrounding synthetic circuit; "
         "whether that is audible depends on the wiring.",
         "bundled:models/fixture.resistor@1"},
        {"fixture.capacitor", 1,
         "Linear capacitance element for coupling, filtering or storage, depending on its wiring.",
         "Not modelled: ideal fixture with no dielectric, leakage or ESR model.",
         "capacitanceF [F]",
         "Synthetic fixture (not a measured or named device; G01 calibration gate open). Ideal linear capacitor q=C*v for solver validation.",
         "Ideal linear capacitor used to validate the circuit solver. It stores charge q equal to C times v. It is a synthetic fixture, not a measured component.",
         "This tooltip describes a synthetic fixture capacitor. Charge equals capacitance times voltage, integrated by the solver at the electrical sample rate. "
         "It has no leakage, equivalent series resistance, dielectric absorption or aging model, so it is a memory element only in the ideal sense. "
         "Its capacitance is bounded by the manifest. Supply-sag behaviour in the interface is a macro handle on a capacitor, not a claim about a physical part. "
         "Real device models remain blocked on measured data (G01).",
         "What changes in audio: capacitance sets time constants and corner frequencies of the surrounding synthetic circuit and the memory of its state.",
         "bundled:models/fixture.capacitor@1"},
        {"fixture.diode", 1,
         "Nonlinear Shockley junction; in the fixtures it acts as the clipping element.",
         "Not modelled: no semiconductor material is declared (no germanium or silicon claim).",
         "saturationCurrentA [A]; emissionCoefficient [1]; thermalVoltageV [V]",
         "Synthetic fixture (not a measured or named device; G01 calibration gate open). Textbook Shockley equation with arbitrary round parameters.",
         "Nonlinear Shockley diode with arbitrary round parameters. It bends the waveform above its conduction knee. It is a synthetic fixture, not a named or measured device.",
         "This tooltip describes a synthetic fixture diode. Current follows the textbook Shockley equation with an exponent cap beyond which the model is linearly extended "
         "so the solver stays stable. The parameters are arbitrary round numbers chosen for validation, not a catalogue device, and no germanium part is represented. "
         "The junction is memoryless by itself; any memory in a circuit comes from capacitors and thermal state around it. "
         "Real device models remain blocked on measured data (G01).",
         "What changes in audio: it limits and adds harmonics when the signal reaches its knee voltage; below the knee the effect is small.",
         "bundled:models/fixture.diode@1"},
        {"fixture.vsource", 1,
         "Ideal voltage source supplying a rail or injected test drive; not a physical component.",
         "Not a component: an ideal source has no material.",
         "voltageV [V]",
         "Synthetic fixture (not a measured or named device; G01 calibration gate open). Ideal DC or arbitrary voltage source.",
         "Ideal voltage source used for rails and the injected drive. It holds its voltage regardless of load. It is a synthetic fixture, not a power supply model.",
         "This tooltip describes a synthetic fixture voltage source. It holds its terminal voltage exactly, with no internal resistance, ripple, sag or recovery behaviour. "
         "The interface sag handle therefore needs a capacitor and load in the circuit to show droop. "
         "The source is bounded by the registry range and is not a model of any mains or battery supply. Real supply models remain blocked on measured data (G01).",
         "What changes in audio: it sets the bias and headroom of the surrounding synthetic circuit; the drive source carries the input signal.",
         "bundled:models/fixture.vsource@1"},
    };
    return cat;
}

const TooltipEntry* findTooltip(const std::string& id, int rev) noexcept {
    for (const auto& e : tooltipCatalogue()) if (id == e.modelId && rev == e.revision) return &e;
    return nullptr;
}

std::string unitsFor(const dsp::DeviceManifest& m) {
    std::string s;
    for (const auto& p : m.params) {
        if (!s.empty()) s += "; ";
        s += p.name + " [" + p.unit + "]";
    }
    return s;
}
std::string unitsFor(const blueprint::SourceModel& s) { return s.paramName + " [V]"; }

int wordCount(const char* t) noexcept {
    int n = 0;
    bool in = false;
    for (; t && *t; ++t) {
        const bool sp = std::isspace(static_cast<unsigned char>(*t)) != 0;
        if (!sp && !in) ++n;
        in = !sp;
    }
    return n;
}

std::string validateEntry(const TooltipEntry& e) {
    const char* f[] = {e.modelId, e.role, e.material, e.units, e.provenance, e.shortText, e.longText, e.whatChangesInAudio, e.sourceLink};
    for (const char* s : f) if (!s || !*s) return "empty field";
    if (wordCount(e.shortText) > 60) return "short text exceeds 60 words";
    if (wordCount(e.longText) > 250) return "long text exceeds 250 words";
    if (std::string(e.provenance).find("ynthetic fixture") == std::string::npos) return "provenance must state synthetic fixture";
    if (std::string(e.whatChangesInAudio).find("What changes in audio") != 0) return "missing 'what changes in audio' section";
    if (std::string(e.sourceLink).rfind("bundled:", 0) != 0) return "source link must be bundled (offline)";
    (void)kProv;
    return {};
}

void TooltipTimer::start(const std::string& id) {
    if (id == target_ && state_ != State::Idle) return;   // same target: keep running
    target_ = id;
    startUs_ = clock_();
    state_ = State::Pending;
    pinned_ = false;
}
void TooltipTimer::leave() {
    if (pinned_ && state_ == State::Open) return;
    state_ = State::Idle;
    target_.clear();
}
void TooltipTimer::escape() {
    pinned_ = false;
    if (state_ == State::Pending || state_ == State::Open) state_ = State::Dismissed;
}
void TooltipTimer::pin() { if (state_ == State::Open) pinned_ = true; }
void TooltipTimer::unpin() { pinned_ = false; }
TooltipTimer::State TooltipTimer::tick() {
    if (state_ == State::Pending && clock_() - startUs_ >= kDelayUs) state_ = State::Open;
    return state_;
}

}  // namespace cd::uimodel
