// CircuitDrift editor shell: Tone State Sphere (UX-07). An expressive summary, NOT a calibrated
// meter; it says so in its label. All inputs are shared telemetry/parameter snapshots; smoothing is
// 80 ms. Edge ripple uses a render-only RNG seeded independently of the audio RNG. Message thread.
#pragma once
#include "ui/shell/ShellCommon.h"

namespace cd::plugin::ui::shell {

struct SphereInputs {
    bool valid = false;               // false: no telemetry yet -> calm ring
    double railVolts = 0, railNominalVolts = 0;
    double outRmsLinear = 0;          // max over channels, linear full scale
    double sag = 0, warmth = 0, age = 0, drift = 0;   // macro values 0..1
    double measuredResidual = -1.0;   // 0..1 normalized nonlinear residual vs calibrated linear
                                      // reference; < 0 = not available (control preview is used)
    bool reduceMotion = false;
};

struct SphereTargets {
    double railFraction = 1.0;        // clamp(rail / V_nom, 0, 1)
    double rmsDb = -120.0;
    double radius = 0.85;             // fraction of the lens radius, 0.85 .. 0.95
    double scaleX = 1.0, scaleY = 1.0;
    double colourT = 0.0;             // 0 cyan, 0.5 amber, 1 crimson
    double edgeAmp = 0.0;             // ripple amplitude as a fraction of radius, <= 0.03
    bool controlPreview = true;       // colour comes from the Warmth control, not a measurement
    bool silent = true;               // calm ring: no pulsing or deformation
};

// UX-07 mapping (pure function, unit-tested).
SphereTargets computeSphereTargets(const SphereInputs& in) noexcept;
juce::Colour sphereColour(double t, const Theme& theme);

class ToneSphere : public juce::Component {
public:
    explicit ToneSphere(ShellContext& ctx);
    // Advances smoothing by dt seconds toward the targets for `in` and repaints.
    void update(const SphereInputs& in, double dtSeconds);
    const SphereTargets& smoothed() const noexcept { return smooth_; }
    const SphereTargets& target() const noexcept { return target_; }
    // Edge radius multiplier at angle theta for the current ripple state (<= 1 + edgeAmp).
    double edgeFactor(double theta) const noexcept;
    void paint(juce::Graphics& g) override;

private:
    ShellContext& ctx_;
    SphereInputs in_;
    SphereTargets target_, smooth_;
    double phase_ = 0.0;
    double phi_[3] = {0, 0, 0}, omega_[3] = {0, 0, 0};
    juce::String describedText_;
    bool primed_ = false;
};

}  // namespace cd::plugin::ui::shell
