// CircuitDrift network: audio bleed (pre-circuit console cross-talk) law (P15/P16, CD-F11).
// Owner: cd_network. Spec 04-instance-network "Audio bleed".
// For receiver i:  c_i = g / max(1, N-1) * sum_{j != i} p_j x_j,  g = 10^(bleed_dB / 20),
// p_j in {-1,+1} a fixed sign seeded from (seed, member UUID) so it does not depend on roster
// order. Level is Off or -90..-24 dB (default -90). An optional topology matrix replaces the
// default weights; it is validated so every absolute row sum <= g and its diagonal is zero, which
// bounds |c_i| <= g * max_j |x_j| for every receiver. Channel rule: same-channel for equal layouts
// (default stereo->stereo L->L, R->R), a mono source duplicates into both stereo channels at -3 dB,
// a stereo source into a mono receiver is 0.5*(L+R); every rule has unit-or-less channel gain.
// x_j is the member's PRE-CIRCUIT tap (after input trim, before its circuit and before received
// bleed), so bleed never re-enters the network (no recursive feedback).
// This models pre-circuit console coupling only; it does not claim other hardware cross-talk.
// Threading: buildBleedWeights is non-audio (fills a caller vector); mixBleed is noexcept,
// allocation free and deterministic (fixed j ascending order).
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>
#include "network/Protocol.h"

namespace cd::network {

inline constexpr double kBleedMinDb = -90.0;
inline constexpr double kBleedMaxDb = -24.0;
inline constexpr double kBleedDefaultDb = -90.0;
inline constexpr double kMonoToStereoGain = 0.70794578438413791;   // 10^(-3/20)

// Receiver-by-source weights over an explicit member order (row-major, size members^2).
struct BleedTopology {
    std::vector<Uuid128> members;
    std::vector<double> weights;
};

struct BleedConfig {
    bool off = false;
    double levelDb = kBleedDefaultDb;
    std::uint64_t seed = 0;
    std::optional<BleedTopology> topology;
};

enum class BleedError : std::uint32_t {
    None = 0, LevelOutOfRange, TopologyNotSquare, TopologyDuplicateMember, TopologyNonFinite,
    TopologyDiagonal, TopologyRowSumExceedsGain
};
const char* toString(BleedError e) noexcept;

double bleedGain(const BleedConfig& c) noexcept;              // 0 when off
double bleedSign(std::uint64_t seed, const Uuid128& member) noexcept;
BleedError validateBleed(const BleedConfig& c) noexcept;

// Effective weights W (N*N, row = receiver) for an ordered roster. Members absent from a
// topology get zero rows/columns. Non-audio (resizes W).
BleedError buildBleedWeights(const BleedConfig& c, std::span<const Uuid128> roster, std::vector<double>& W);
// Max absolute row sum of W (diagnostics/tests).
double maxAbsRowSum(std::span<const double> W, std::size_t n) noexcept;

struct BleedSource {
    const double* ch[kMaxChannels] = {nullptr, nullptr};  // nullptr = silent/missing
    std::uint32_t channels = 0;
    double gain = 1.0;                                     // contribution ramp 0..1
};
struct BleedSink {
    double* ch[kMaxChannels] = {nullptr, nullptr};
    std::uint32_t channels = 0;
    bool enabled = true;                                   // receiver opted in; else zeros
};

// sinks[i] = sum_j W[i*n + j] * gain_j * route(x_j). Sinks are overwritten. noexcept.
void mixBleed(std::span<const double> W, std::size_t n, std::span<const BleedSource> sources,
              std::span<const BleedSink> sinks, std::size_t frames) noexcept;

}  // namespace cd::network
