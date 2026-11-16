// CircuitDrift network: bleed law (see Bleed.h).
#include "network/Bleed.h"
#include <cmath>

namespace cd::network {

const char* toString(BleedError e) noexcept {
    switch (e) {
        case BleedError::None: return "ok";
        case BleedError::LevelOutOfRange: return "bleed level must be Off or -90..-24 dB";
        case BleedError::TopologyNotSquare: return "topology matrix must be members x members";
        case BleedError::TopologyDuplicateMember: return "topology lists a member twice";
        case BleedError::TopologyNonFinite: return "topology weight is not finite";
        case BleedError::TopologyDiagonal: return "topology diagonal must be zero (no self bleed)";
        case BleedError::TopologyRowSumExceedsGain: return "topology absolute row sum exceeds bleed gain g";
    }
    return "unknown";
}

double bleedGain(const BleedConfig& c) noexcept {
    if (c.off) return 0.0;
    return std::pow(10.0, c.levelDb / 20.0);
}

double bleedSign(std::uint64_t seed, const Uuid128& m) noexcept {
    std::uint64_t z = seed ^ (m.hi * 0x9E3779B97F4A7C15ull) ^ ((m.lo << 31) | (m.lo >> 33));
    z += 0x9E3779B97F4A7C15ull;                      // splitmix64 finaliser
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return (z & 1u) ? 1.0 : -1.0;
}

BleedError validateBleed(const BleedConfig& c) noexcept {
    if (!c.off && !(c.levelDb >= kBleedMinDb && c.levelDb <= kBleedMaxDb)) return BleedError::LevelOutOfRange;
    if (!c.topology) return BleedError::None;
    const auto& t = *c.topology;
    const std::size_t n = t.members.size();
    if (t.weights.size() != n * n) return BleedError::TopologyNotSquare;
    for (std::size_t a = 0; a < n; ++a)
        for (std::size_t b = a + 1; b < n; ++b)
            if (t.members[a] == t.members[b]) return BleedError::TopologyDuplicateMember;
    const double g = bleedGain(c);
    for (std::size_t i = 0; i < n; ++i) {
        double row = 0;
        for (std::size_t j = 0; j < n; ++j) {
            const double w = t.weights[i * n + j];
            if (!std::isfinite(w)) return BleedError::TopologyNonFinite;
            if (i == j && w != 0.0) return BleedError::TopologyDiagonal;
            row += std::fabs(w);
        }
        if (row > g * (1.0 + 1e-12)) return BleedError::TopologyRowSumExceedsGain;
    }
    return BleedError::None;
}

BleedError buildBleedWeights(const BleedConfig& c, std::span<const Uuid128> roster, std::vector<double>& W) {
    const BleedError e = validateBleed(c);
    const std::size_t n = roster.size();
    W.assign(n * n, 0.0);
    if (e != BleedError::None) return e;
    const double g = bleedGain(c);
    if (g == 0.0) return BleedError::None;
    if (!c.topology) {
        const double scale = g / double(n > 1 ? n - 1 : 1);
        for (std::size_t j = 0; j < n; ++j) {
            const double pj = bleedSign(c.seed, roster[j]) * scale;
            for (std::size_t i = 0; i < n; ++i)
                if (i != j) W[i * n + j] = pj;
        }
        return BleedError::None;
    }
    const auto& t = *c.topology;
    const std::size_t m = t.members.size();
    std::vector<std::size_t> idx(n, m);
    for (std::size_t r = 0; r < n; ++r)
        for (std::size_t a = 0; a < m; ++a)
            if (t.members[a] == roster[r]) { idx[r] = a; break; }
    for (std::size_t i = 0; i < n; ++i) {
        if (idx[i] == m) continue;
        for (std::size_t j = 0; j < n; ++j)
            if (i != j && idx[j] != m) W[i * n + j] = t.weights[idx[i] * m + idx[j]];
    }
    return BleedError::None;
}

double maxAbsRowSum(std::span<const double> W, std::size_t n) noexcept {
    double best = 0;
    for (std::size_t i = 0; i < n; ++i) {
        double row = 0;
        for (std::size_t j = 0; j < n; ++j) row += std::fabs(W[i * n + j]);
        if (row > best) best = row;
    }
    return best;
}

void mixBleed(std::span<const double> W, std::size_t n, std::span<const BleedSource> sources,
              std::span<const BleedSink> sinks, std::size_t frames) noexcept {
    if (frames > kQuantumFrames * 1024) frames = 0;
    for (std::size_t i = 0; i < sinks.size() && i < n; ++i) {
        const BleedSink& out = sinks[i];
        const std::uint32_t oc = out.channels > kMaxChannels ? kMaxChannels : out.channels;
        for (std::uint32_t c = 0; c < oc; ++c)
            if (out.ch[c])
                for (std::size_t s = 0; s < frames; ++s) out.ch[c][s] = 0.0;
        if (!out.enabled) continue;
        for (std::size_t j = 0; j < sources.size() && j < n; ++j) {
            if (j == i) continue;                 // never own bleed
            const BleedSource& src = sources[j];
            const double w = W[i * n + j] * src.gain;
            if (w == 0.0 || src.channels == 0 || !src.ch[0]) continue;
            if (src.channels == oc) {
                for (std::uint32_t c = 0; c < oc; ++c)
                    if (out.ch[c] && src.ch[c])
                        for (std::size_t s = 0; s < frames; ++s) out.ch[c][s] += w * src.ch[c][s];
            } else if (src.channels == 1 && oc == 2) {
                const double wm = w * kMonoToStereoGain;
                for (std::uint32_t c = 0; c < 2; ++c)
                    if (out.ch[c])
                        for (std::size_t s = 0; s < frames; ++s) out.ch[c][s] += wm * src.ch[0][s];
            } else if (src.channels == 2 && oc == 1 && src.ch[1] && out.ch[0]) {
                const double wh = 0.5 * w;
                for (std::size_t s = 0; s < frames; ++s) out.ch[0][s] += wh * (src.ch[0][s] + src.ch[1][s]);
            }
        }
    }
}

}  // namespace cd::network
