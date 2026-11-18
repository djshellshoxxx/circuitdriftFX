// cd_control: Age/Sag/Warmth/Drift macro mapping (CD-F06, 02-musical-controls "Factory macro mappings").
// Pure, one-way: macro values in -> SlotModulation contributions out. Never writes host parameters.
// Mappings are authored creative design choices, not physical laws (spec). Threading: pure functions,
// noexcept, no allocation; callable from any thread including the audio thread.
#pragma once
#include "core/Modulation.h"

namespace cd::control {

struct MacroInputs {
    double age = 0, sag = 0, drift = 0, warmth = 0;  // normalized, clamped to [0,1] (NaN -> 0)
    bool analogNoiseEnabled = false;
};

// Writes the macro-owned fields of `slot` (age, tolerance, leakage, hf shelf, age noise, reservoir
// divisor, sag macro, warmth drive/bias/flux, thermal sensitivity). Does not touch creative cutoff/delay,
// tempo recovery or sidechain mode (owned by other control modules).
void applyMacros(const MacroInputs& in, core::SlotModulation& slot) noexcept;

}  // namespace cd::control
