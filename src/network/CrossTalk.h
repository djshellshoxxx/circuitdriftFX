// CircuitDrift network: audio-bleed cross-talk math and the shared PSU reservoir (CD-F11).
// Owner: cd_network. Shared by the live broker and the deterministic co-render harness so both
// paths use identical arithmetic. All functions noexcept, no allocation.
//
// Bleed: g = 10^(dB/20), level Off or -90..-24 dB (default -90). For receiver i (default topology)
//   c_i = g / max(1, N-1) * sum_{j != i} p_j * w_j * x_j
// with p_j a fixed seeded sign (+-1) and w_j the 50 ms join/leave ramp. Channel mapping: same
// channel for equal layouts; mono source into a stereo receiver duplicates at -3 dB (1/sqrt 2);
// stereo source into a mono receiver uses (L+R)/2. An optional topology matrix must have absolute
// row sum <= g: offending rows are scaled down to exactly g and flagged (non-finite entries -> 0).
// Scope: models pre-circuit console coupling only, not every hardware cross-talk path.
//
// PSU: SYNTHETIC reservoir fixture (Thevenin source V_nom/R into C, exact exponential update per
// quantum). It is a labelled placeholder model, not a validated supply (G01/G02). The circuits in
// cd_runtime have no supply input yet, so the shared rail is telemetry/influence data only.
#pragma once
#include <cstdint>
#include <string>
#include "network/Protocol.h"

namespace cd::network {

struct BleedConfig {
    bool enabled = false;          // opt-in separately from shared demand
    double levelDb = -90.0;        // clamped to [-90, -24]
};
double bleedGain(const BleedConfig& c) noexcept;            // 0 when disabled
double memberSign(std::uint64_t seed, const Uuid& member) noexcept;   // fixed seeded +-1

using TopologyMatrix = double[kMaxMembers][kMaxMembers];     // [receiver][source], by entry slot
// Enforces sum_{j != i} |M[i][j]| <= g for i < n (diagonal forced to 0). Returns true if any row
// was clamped or contained a non-finite entry.
bool enforceRowSums(TopologyMatrix& m, std::uint32_t n, double g) noexcept;
// Absolute row sum of row i over the first n columns (diagonal excluded).
double rowAbsSum(const TopologyMatrix& m, std::uint32_t i, std::uint32_t n) noexcept;

// One quantum of one source: channel pointers (ch[1] ignored when channels == 1), sign, and the
// per-sample influence weight (nullptr = 1). present=false contributes zero new audio.
struct BleedSource {
    const double* ch[kMaxChannels] = {nullptr, nullptr};
    std::uint32_t channels = 0;
    double sign = 1.0;
    const double* weight = nullptr;
    bool present = false;
};
// Mapped sample of source s for a receiver with recvChannels at channel c, frame n.
double mappedSample(const BleedSource& s, std::uint32_t recvChannels, std::uint32_t c, std::uint32_t n) noexcept;
// Writes the bleed for receiver `i` (out[c][0..frames)) using either the default topology (matrix
// == nullptr) or matrix row i (already enforced). src has `slots` entries indexed by slot; only
// present sources contribute. rosterSize = N (active members, present or missing) for g/(N-1).
void mixBleed(const BleedSource* src, std::uint32_t slots, std::uint32_t rosterSize, std::uint32_t i,
              std::uint32_t recvChannels, double g, const TopologyMatrix* matrix, double* const* out,
              std::uint32_t frames) noexcept;

struct PsuManifest {
    std::string id = "synthetic-reservoir-v1";   // SYNTHETIC fixture id
    std::uint64_t railModelId = 0x5359'4e52'4149'4c31ull;   // 'SYNRAIL1'
    double nominalV = 250.0;
    double sourceOhm = 50.0;
    double capacitanceF = 100e-6;
    std::uint64_t hash() const noexcept;
    bool valid() const noexcept;
};

class SharedReservoir {
public:
    bool prepare(const PsuManifest& m, double sampleRate) noexcept;
    void reset() noexcept { v_ = m_.nominalV; }
    // Advances one quantum with total demand current [A] (clamped finite >= 0).
    double step(double currentA) noexcept;
    // Value step() would produce from voltage v0 (no state change).
    double peek(double v0, double currentA) const noexcept;
    double voltage() const noexcept { return v_; }
    void setVoltage(double v) noexcept { v_ = v; }
    double quantumSeconds() const noexcept { return dt_; }
    const PsuManifest& manifest() const noexcept { return m_; }
private:
    PsuManifest m_{};
    double v_ = 0, dt_ = 0, decay_ = 0;
};

// Co-render fixed-point joint solve for demand that depends on rail (I = P / V): at most
// maxIterations (<= 8) iterations; converged when |dV| <= 1e-12 * V_nom.
struct RailSolve { double v = 0; int iterations = 0; bool converged = false; };
RailSolve solveRailFixedPoint(const SharedReservoir& r, double totalPowerW, int maxIterations = 8) noexcept;

}  // namespace cd::network
