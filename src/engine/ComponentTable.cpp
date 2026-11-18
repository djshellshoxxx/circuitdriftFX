#include <cstring>
#include "engine/EngineTypes.h"

namespace cd::engine {
namespace {
#define CD_SYNTH "synthetic-fixture, not calibrated hardware (G01)"
using R = ComponentRole;
using S = ComponentScope;
// Static component table for inspector/tooltips/faults/timeline. Indexed by ComponentId; IDs are
// permanent. Material strings describe the synthetic model family, not a sourced part.
const ComponentInfo kTable[kComponentCount] = {
    {ComponentId::C_in, "C_in", R::InputCoupling, S::PerChannel, "Input coupling capacitor", "film/paper (tubes, JFET) or electrolytic (BJT)", "F",
     "synthetic.film-capacitor | synthetic.electrolytic", CD_SYNTH "; ESR + leakage form"},
    {ComponentId::R_g, "R_g", R::GridLeakBias, S::PerChannel, "Grid leak / bias network", "resistorType (carbon composition, metal film, wirewound)", "Ohm",
     "fixture.resistor", CD_SYNTH "; BJT uses an R_top/R_g divider from local B+"},
    {ComponentId::V1, "V1", R::ActiveElement, S::PerChannel, "Active gain element", "Triode12AX7 / PentodeEF86 / BJT / JFET (synthetic families)", "A",
     "synthetic.triode-12ax7 | synthetic.pentode-ef86 | synthetic.npn-small-signal | synthetic.njfet-small-signal",
     CD_SYNTH "; Koren / Ebers-Moll / Shichman-Hodges equation forms with round constants"},
    {ComponentId::R_load, "R_load", R::PlateLoad, S::PerChannel, "Plate/collector/drain load resistor", "resistorType", "Ohm",
     "fixture.resistor", CD_SYNTH},
    {ComponentId::R_k, "R_k", R::CathodeResistor, S::PerChannel, "Cathode/emitter/source resistor", "resistorType", "Ohm",
     "fixture.resistor", CD_SYNTH},
    {ComponentId::C_k, "C_k", R::CathodeBypass, S::PerChannel, "Cathode/emitter bypass electrolytic", "aluminium electrolytic", "F",
     "synthetic.electrolytic", CD_SYNTH "; BJT branch includes 220 Ohm series degeneration"},
    {ComponentId::C_out, "C_out", R::OutputCoupling, S::PerChannel, "Output coupling electrolytic", "aluminium electrolytic", "F",
     "synthetic.electrolytic", CD_SYNTH "; leakage passes DC into the transformer primary"},
    {ComponentId::T_out, "T_out", R::OutputTransformer, S::PerChannel, "Output transformer (magnetizing branch)", "core: Nickel / Steel / Iron", "Wb",
     "synthetic.output-transformer", CD_SYNTH "; anhysteretic flux-state saturation + linear core loss (dynamic B-H only)"},
    {ComponentId::C_dec, "C_dec", R::Decoupling, S::PerChannel, "Local B+ decoupling electrolytic", "aluminium electrolytic", "F",
     "synthetic.electrolytic", CD_SYNTH},
    {ComponentId::R_dec, "R_dec", R::DecouplingResistor, S::PerChannel, "Local B+ decoupling resistor", "resistorType", "Ohm",
     "fixture.resistor", CD_SYNTH},
    {ComponentId::R_g2, "R_g2", R::ScreenResistor, S::PerChannel, "Screen-grid resistor (pentode only)", "resistorType", "Ohm",
     "fixture.resistor", CD_SYNTH},
    {ComponentId::C_g2, "C_g2", R::ScreenBypass, S::PerChannel, "Screen-grid bypass (pentode only)", "film", "F",
     "synthetic.electrolytic", CD_SYNTH},
    {ComponentId::C_res, "C_res", R::Reservoir, S::PerSlot, "Reservoir capacitor", "aluminium electrolytic", "F",
     "reduced-supply:reservoir", CD_SYNTH "; reduced supply model C dV/dt = I_rect - I_load"},
    {ComponentId::D_rect, "D_rect", R::Rectifier, S::PerSlot, "Rectifier", "Tube5U4G / Selenium / SiliconDiode (per-unit laws)", "A",
     "reduced-supply:rectifier", CD_SYNTH "; per-unit I-V law, lumped junction recovery"},
    {ComponentId::R_filt, "R_filt", R::FilterResistor, S::PerSlot, "Supply filter resistor (CRC)", "wirewound", "Ohm",
     "reduced-supply:filter", CD_SYNTH},
    {ComponentId::C_filt, "C_filt", R::FilterCapacitor, S::PerSlot, "Supply filter capacitor (CRC)", "aluminium electrolytic", "F",
     "reduced-supply:filter", CD_SYNTH},
    {ComponentId::Chassis, "Chassis", R::Chassis, S::PerSlot, "Chassis thermal node", "steel chassis (thermal only)", "K",
     "thermal-only", CD_SYNTH "; heat-sink node of the thermal network"},
};
#undef CD_SYNTH
}  // namespace

const ComponentInfo* componentTable() noexcept { return kTable; }

const ComponentInfo* findComponent(const char* key) noexcept {
    if (!key) return nullptr;
    for (const auto& c : kTable) if (std::strcmp(c.key, key) == 0) return &c;
    return nullptr;
}

}  // namespace cd::engine
