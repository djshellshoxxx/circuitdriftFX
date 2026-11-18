#include "control/MacroMapper.h"

namespace cd::control {
namespace {
double unit(double v) noexcept { return (v >= 0.0) ? (v > 1.0 ? 1.0 : v) : 0.0; }  // NaN -> 0
}

void applyMacros(const MacroInputs& in, core::SlotModulation& s) noexcept {
    const double a = unit(in.age), sg = unit(in.sag), d = unit(in.drift), w = unit(in.warmth);
    s.ageYearsMin = 60.0 * a;
    s.toleranceSigmaAdd = 0.08 * a;
    s.leakageAdd = 0.1 * a * a;
    s.hfShelfDb = -6.0 * a;
    s.ageNoiseDbfs = in.analogNoiseEnabled ? (-90.0 + 24.0 * a) : -200.0;
    s.reservoirCapDivisor = 1.0 + 3.0 * sg;
    s.sagMacro = sg;
    s.driveOffsetDb = 18.0 * w;
    s.biasOffset = 0.08 * w;
    s.fluxDriveFactor = 1.0 + 0.5 * w;
    s.thermalSensitivityScale = 1.0 + 4.0 * d;
}

}  // namespace cd::control
