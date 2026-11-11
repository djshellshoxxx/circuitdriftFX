// CircuitDrift network: cross-talk math and synthetic shared reservoir (see CrossTalk.h).
// Owner: cd_network. Threading: pure / per-object; noexcept, no allocation.
#include "network/CrossTalk.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace cd::network {

double bleedGain(const BleedConfig& c) noexcept {
    if (!c.enabled || !std::isfinite(c.levelDb)) return 0.0;
    const double db = std::clamp(c.levelDb, -90.0, -24.0);
    return std::pow(10.0, db / 20.0);
}

double memberSign(std::uint64_t seed, const Uuid& member) noexcept {
    Hash64 h;
    h.u64(seed);
    h.update(member.b.data(), 16);
    return (h.finish() & 1u) ? -1.0 : 1.0;
}

double rowAbsSum(const TopologyMatrix& m, std::uint32_t i, std::uint32_t n) noexcept {
    double s = 0;
    for (std::uint32_t j = 0; j < n && j < kMaxMembers; ++j)
        if (j != i) s += std::fabs(m[i][j]);
    return s;
}

bool enforceRowSums(TopologyMatrix& m, std::uint32_t n, double g) noexcept {
    n = std::min(n, kMaxMembers);
    if (!std::isfinite(g) || g < 0) g = 0;
    bool flagged = false;
    for (std::uint32_t i = 0; i < n; ++i) {
        m[i][i] = 0;
        for (std::uint32_t j = 0; j < n; ++j)
            if (!std::isfinite(m[i][j])) { m[i][j] = 0; flagged = true; }
        const double s = rowAbsSum(m, i, n);
        if (s > g) {
            // Scale slightly below g/s so rounding in the scaled sum cannot exceed g; re-check.
            const double k = (g / s) * (1.0 - 64.0 * std::numeric_limits<double>::epsilon());
            for (std::uint32_t j = 0; j < n; ++j) m[i][j] *= k;
            for (int guard = 0; guard < 64 && rowAbsSum(m, i, n) > g; ++guard)
                for (std::uint32_t j = 0; j < n; ++j) m[i][j] *= 1.0 - 64.0 * std::numeric_limits<double>::epsilon();
            flagged = true;
        }
    }
    return flagged;
}

double mappedSample(const BleedSource& s, std::uint32_t recvChannels, std::uint32_t c, std::uint32_t n) noexcept {
    if (!s.present || s.channels == 0) return 0.0;
    if (s.channels == 1) return recvChannels == 2 ? s.ch[0][n] * 0.70710678118654752 : s.ch[0][n];
    if (recvChannels == 1) return 0.5 * (s.ch[0][n] + s.ch[1][n]);
    return s.ch[c][n];
}

void mixBleed(const BleedSource* src, std::uint32_t slots, std::uint32_t rosterSize, std::uint32_t i,
              std::uint32_t recvChannels, double g, const TopologyMatrix* matrix, double* const* out,
              std::uint32_t frames) noexcept {
    frames = std::min(frames, kQuantum);
    recvChannels = std::clamp(recvChannels, 1u, kMaxChannels);
    const std::uint32_t n = std::min(slots, kMaxMembers);
    // Divisor is never smaller than the number of contributing peers, so the default topology's
    // absolute row sum is <= g even if a caller passes an inconsistent rosterSize.
    std::uint32_t others = 0;
    for (std::uint32_t j = 0; j < n; ++j) others += (j != i && src[j].present) ? 1u : 0u;
    const std::uint32_t div = std::max({1u, rosterSize > 0 ? rosterSize - 1 : 0u, others});
    const double k = g / double(div);
    for (std::uint32_t c = 0; c < recvChannels; ++c) {
        double* o = out[c];
        for (std::uint32_t f = 0; f < frames; ++f) o[f] = 0.0;
        // Fixed source order (slot index) keeps the floating-point sum deterministic.
        for (std::uint32_t j = 0; j < n; ++j) {
            if (j == i || !src[j].present) continue;
            const double coef = matrix ? (*matrix)[i][j] : k * src[j].sign;
            if (coef == 0.0) continue;
            for (std::uint32_t f = 0; f < frames; ++f) {
                const double w = src[j].weight ? src[j].weight[f] : 1.0;
                o[f] += coef * w * mappedSample(src[j], recvChannels, c, f);
            }
        }
    }
}

std::uint64_t PsuManifest::hash() const noexcept {
    Hash64 h;
    h.update(id.data(), id.size());
    h.u64(railModelId);
    h.f64(nominalV);
    h.f64(sourceOhm);
    h.f64(capacitanceF);
    return h.finish();
}

bool PsuManifest::valid() const noexcept {
    return std::isfinite(nominalV) && nominalV > 0 && nominalV < 1e4 && std::isfinite(sourceOhm) && sourceOhm > 0 &&
           sourceOhm < 1e6 && std::isfinite(capacitanceF) && capacitanceF > 0 && capacitanceF < 10.0;
}

bool SharedReservoir::prepare(const PsuManifest& m, double sampleRate) noexcept {
    if (!m.valid() || !std::isfinite(sampleRate) || sampleRate <= 0) return false;
    m_ = m;
    dt_ = double(kQuantum) / sampleRate;
    decay_ = std::exp(-dt_ / (m.sourceOhm * m.capacitanceF));
    v_ = m.nominalV;
    return true;
}

double SharedReservoir::peek(double v0, double currentA) const noexcept {
    if (!std::isfinite(currentA) || currentA < 0) currentA = 0;
    currentA = std::min(currentA, m_.nominalV / m_.sourceOhm);   // cannot draw below 0 V steady state
    const double vInf = m_.nominalV - currentA * m_.sourceOhm;
    return vInf + (v0 - vInf) * decay_;
}

double SharedReservoir::step(double currentA) noexcept {
    v_ = peek(v_, currentA);
    return v_;
}

RailSolve solveRailFixedPoint(const SharedReservoir& r, double totalPowerW, int maxIterations) noexcept {
    maxIterations = std::clamp(maxIterations, 1, 8);
    if (!std::isfinite(totalPowerW) || totalPowerW < 0) totalPowerW = 0;
    RailSolve s;
    const double vMin = 1e-3 * r.manifest().nominalV;
    double v = std::max(r.voltage(), vMin);
    for (int it = 1; it <= maxIterations; ++it) {
        const double next = std::max(r.peek(r.voltage(), totalPowerW / v), vMin);
        s.iterations = it;
        const bool done = std::fabs(next - v) <= 1e-12 * r.manifest().nominalV;
        v = next;
        if (done) { s.converged = true; break; }
    }
    s.v = v;
    return s;
}

}  // namespace cd::network
