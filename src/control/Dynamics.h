// Transient recovery detector and external-sidechain load detector (P05; CD-F15/F16).
// Owner: cd_control. Threading: audio thread after prepare(); noexcept, no allocation, double state.
// Neither class produces audio: they return only a control scalar plus status flags.
#pragma once
#include "dsp/OutputProtection.h"

namespace cd::control {

// Linked causal transient-evidence detector. process() returns the held evidence b in [0,1];
// P = amount*b ; r_eff = r + P(1-r). Non-finite input is sanitized to 0 and flagged.
class TransientDetector {
public:
    bool prepare(double sampleRate) noexcept;   // false (state unchanged) if fs invalid
    void reset() noexcept;
    double process(double l, double r) noexcept;
    bool sawNonFinite() const noexcept { return sanitized_; }
    static double strength(double amount, double b) noexcept;       // P = clamp(amount,0,1)*b
    static double effectiveR(double r, double P) noexcept;          // r + P(1-r)
private:
    double aFA_ = 0, aFR_ = 0, aSA_ = 0, aSR_ = 0, aHR_ = 0;
    double fast_ = 0, slow_ = 0, held_ = 0;
    int holdLeft_ = 0, holdSamples_ = 0;
    bool sanitized_ = false;
};

struct SidechainStatus { bool disconnected = false; bool sanitized = false; };

// I_ext = depth*I_budget*clamp(E/E_ref,0,4); E_ref = -18 dBFS RMS. E is an RMS-style level: x^2 is
// smoothed (10 ms, to avoid peak-biased ripple), then a 1 ms attack / 50 ms release envelope, sqrt.
// Main source -> zero external current (main path uses 3 s I_dyn, see composeLoadCurrent).
class SidechainLoadDetector {
public:
    enum class Source { Main, External, Blend };
    struct Config { Source source = Source::External; double hpfHz = 40, depth = 0, iBudgetAmps = 0; };
    bool prepare(double sampleRate, const Config& c) noexcept;  // clamps hpf 20..300, depth 0..2
    void setConnected(bool c) noexcept { connected_ = c; }
    void reset() noexcept;
    // extL/extR: external bus sample. Returns I_ext in amperes (>= 0).
    double process(double extL, double extR) noexcept;
    SidechainStatus status() const noexcept { return {!connected_ && cfg_.source != Source::Main, sanitized_}; }
private:
    Config cfg_;
    dsp::ButterworthHpf hpfL_, hpfR_;
    double aMs_ = 0, aA_ = 0, aR_ = 0, ms_ = 0, env_ = 0;
    bool connected_ = true, sanitized_ = false;
};

}  // namespace cd::control
