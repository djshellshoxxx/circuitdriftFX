// cd_control: causal linked transient detector (CD-F15). Produces per-sample preservation strength
// P=amount*b consumed by the engine as r_effective=r+P*(1-r). The physical reservoir charge is never
// altered here. Creative transient protection, not zero-lookahead attack preservation (spec).
//
// p=max(|L|,|R|); fast env attack 0.2 ms/release 10 ms, slow env attack 20 ms/release 100 ms;
// b=clamp((20log10((fast+1e-9)/(slow+1e-9))-3)/9,0,1), forced to 0 while fast env < -60 dBFS;
// b is peak-held 5 ms then released over 20 ms (one-pole).
//
// Threading: prepare() any thread; process() noexcept, allocation-free, audio thread. State advances
// per sample so any block partition gives identical output (<=1e-12). amount==0 yields exactly 0.
#pragma once

namespace cd::control {

class TransientDetector {
public:
    struct State {
        double sampleRate = 0;
        double fast = 0, slow = 0, held = 0;
        int holdCount = 0;
    };
    void prepare(double sampleRate) noexcept;
    void reset() noexcept;
    State save() const noexcept { return {fs_, fast_, slow_, held_, holdCount_}; }
    bool canRestore(const State& state) const noexcept;
    bool restore(const State& state) noexcept;  // rejection changes nothing
    // R may be null (mono). Writes `frames` samples of P into out.
    void process(const double* L, const double* R, int frames, double amount, double* out) noexcept;

private:
    double fs_ = 0;  // zero identifies an unprepared coefficient set
    double aFastA_ = 0, aFastR_ = 0, aSlowA_ = 0, aSlowR_ = 0, aHoldRel_ = 0;
    int holdSamples_ = 0;
    double fast_ = 0, slow_ = 0, held_ = 0;
    int holdCount_ = 0;
};

}  // namespace cd::control
