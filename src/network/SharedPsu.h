// CircuitDrift network: SYNTHETIC shared reservoir supply model (P15/P16, CD-F11).
// Owner: cd_network. This is a labelled SYNTHETIC fixture, not a validated power-supply model
// (G01 stays open): an ideal rectified source V_src behind a source resistance R_s charging a
// reservoir C (diode conducts only while V < V_src, no ripple):
//     C dV/dt = I_rect - I_load,   I_rect = max(0, (V_src - V) / R_s)
// solved in closed form over one 128-frame quantum with the load held constant. The rail
// fraction r = V / V_nom is a READOUT (no circuit consumes it yet).
// The broker owns one instance per group (shared demand); every NetworkMember runs its own
// instance on its own demand as the independently running local PSU it fades to.
// Threading: construction non-audio; stepQuantum/trial/reset are noexcept and allocation free.
#pragma once
#include <cstdint>

namespace cd::network {

inline constexpr char kPsuModelVersion[] = "cd.network.psu.synthetic-reservoir/1";

struct PsuSpec {
    std::uint64_t manifestId = 0x5359'4E50'5355'0001ull;    // compatibility key ("SYNPSU" 1)
    std::uint64_t railModelId = 0x5241'494C'0001ull;        // rail model identity
    double nominalRailV = 300.0;
    double sourceV = 310.0;
    double sourceResistanceOhm = 50.0;
    double capacitanceF = 100e-6;
};
bool validPsuSpec(const PsuSpec& s) noexcept;

class ReservoirPsu {
public:
    ReservoirPsu() = default;
    ReservoirPsu(const PsuSpec& spec, double sampleRate) noexcept;
    void reset() noexcept { volts_ = spec_.nominalRailV; }
    // Volts after one quantum starting from v0 with constant load (pure; used by fixed-point).
    double trial(double v0, double loadA) const noexcept;
    // Advances the state one quantum; returns r = V / V_nom.
    double stepQuantum(double loadA) noexcept;
    double volts() const noexcept { return volts_; }
    void setVolts(double v) noexcept { volts_ = v; }
    double railFraction() const noexcept { return spec_.nominalRailV > 0 ? volts_ / spec_.nominalRailV : 0; }
    double fractionOf(double v) const noexcept { return spec_.nominalRailV > 0 ? v / spec_.nominalRailV : 0; }
    const PsuSpec& spec() const noexcept { return spec_; }

private:
    PsuSpec spec_{};
    double dt_ = 0;      // quantum duration [s]
    double tau_ = 1;     // R_s C
    double volts_ = 300.0;
};

}  // namespace cd::network
