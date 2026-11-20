// CircuitDrift core: control -> engine modulation contract (docs/ARCHITECTURE.md).
// Produced by cd_control each block, consumed by cd_engine. Plain units; defaults are identity
// (no contribution). POD only: safe to copy on the audio thread, no allocation.
#pragma once
#include <cstdint>

namespace cd::core {

struct SlotModulation {
    double ageYearsMin = 0;            // effective age = max(base age, ageYearsMin)       (Age: 60a)
    double toleranceSigmaAdd = 0;      // fraction added to tolerance sigma                (Age: 0.08a)
    double leakageAdd = 0;             // fraction of manifest safe leakage range          (Age: 0.1a^2)
    double hfShelfDb = 0;              // creative 6 kHz shelf gain, Q 0.707               (Age: -6a)
    double ageNoiseDbfs = -200;        // Age noise target; <= -200 means none (gated by analogNoise.enabled)
    double reservoirCapDivisor = 1;    // C_eff = max(C_min, C_base / divisor)             (Sag: 1+3s)
    double sagMacro = 0;               // s in [0,1]; main-load augmentation 3*s*I_dyn
    double driveOffsetDb = 0;          // Warmth 18w plus harmonic-targeting delta (targeting bounded +-6 dB)
    double biasOffset = 0;             // fraction of manifest bias span (Warmth 0.08w plus targeting)
    double fluxDriveFactor = 1;        // Warmth 1+0.5w, within safe transformer domain
    double thermalSensitivityScale = 1; // Drift 1+4d
    double creativeCutoffOct = 0;      // creative cutoff motion in octaves (drift, tempo, key quantized)
    double creativeDelayMs = 0;        // pitch-drift deviation around the 1.5 ms base delay
    bool creativeDelayActive = false;  // pitchDrift.enabled and not zero-latency
    double recoveryTauS = 0;           // tempo-synced supply recovery tau [s]; 0 = natural recovery
    int sidechainMode = 0;             // 0 Main, 1 External, 2 Blend (02-musical-controls precedence)
};

struct ModulationBlock {
    SlotModulation slot[2];
    const double* externalLoadA = nullptr;      // per-sample external load current [A], length frames, or null
    const double* transientPreserve = nullptr;  // per-sample preservation strength P in [0,1], or null
};

}  // namespace cd::core
