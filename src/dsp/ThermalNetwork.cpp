#include "ThermalNetwork.h"
#include <cmath>
#include <utility>

namespace cd::dsp {

bool ThermalNetwork::prepare(const Config& cfg, double dt, double ambientK) noexcept {
    const std::size_t n = cfg.n;
    if (n == 0 || n > kMaxNodes || !(dt > 0.0) || !std::isfinite(dt) || !(ambientK > 0.0)) return false;
    for (std::size_t i = 0; i < n; ++i) {
        if (!(cfg.capacity[i] > 0.0) || !std::isfinite(cfg.capacity[i])) return false;
        if (!(cfg.toAmbient[i] >= 0.0) || !std::isfinite(cfg.toAmbient[i])) return false;
        for (std::size_t j = 0; j < n; ++j) {
            const double g = cfg.coupling[i][j];
            if (!(g >= 0.0) || !std::isfinite(g) || g != cfg.coupling[j][i]) return false;
            if (i == j && g != 0.0) return false;
        }
    }
    // Every thermal island (connected component under G_ij>0) needs an ambient path (spec rule).
    {
        std::array<bool, kMaxNodes> seen{};
        std::array<std::size_t, kMaxNodes> stack{};
        for (std::size_t s0 = 0; s0 < n; ++s0) {
            if (seen[s0]) continue;
            bool grounded = false;
            std::size_t sp = 0;
            stack[sp++] = s0; seen[s0] = true;
            while (sp) {
                const std::size_t u = stack[--sp];
                grounded = grounded || cfg.toAmbient[u] > 0.0;
                for (std::size_t v = 0; v < n; ++v)
                    if (!seen[v] && cfg.coupling[u][v] > 0.0) { seen[v] = true; stack[sp++] = v; }
            }
            if (!grounded) return false;
        }
    }
    // Build A and invert by Gauss-Jordan with partial pivoting (constant per preparation).
    // Local arrays so a failed prepare leaves the live object unchanged.
    std::array<std::array<double, 2 * kMaxNodes>, kMaxNodes> m{};
    for (std::size_t i = 0; i < n; ++i) {
        double diag = cfg.capacity[i] / dt + cfg.toAmbient[i];
        for (std::size_t j = 0; j < n; ++j) {
            diag += cfg.coupling[i][j];
            m[i][j] = -cfg.coupling[i][j];
        }
        m[i][i] = diag;
        m[i][n + i] = 1.0;
    }
    for (std::size_t c = 0; c < n; ++c) {
        std::size_t piv = c;
        for (std::size_t r = c + 1; r < n; ++r)
            if (std::fabs(m[r][c]) > std::fabs(m[piv][c])) piv = r;
        if (!(std::fabs(m[piv][c]) > 1e-300)) return false;  // disconnected island
        std::swap(m[c], m[piv]);
        const double d = 1.0 / m[c][c];
        for (std::size_t k = 0; k < 2 * n; ++k) m[c][k] *= d;
        for (std::size_t r = 0; r < n; ++r) {
            if (r == c) continue;
            const double f = m[r][c];
            if (f == 0.0) continue;
            for (std::size_t k = 0; k < 2 * n; ++k) m[r][k] -= f * m[c][k];
        }
    }
    n_ = n;
    ambient_ = ambientK;
    for (std::size_t i = 0; i < n; ++i) {
        cOverDt_[i] = cfg.capacity[i] / dt;
        gAmb_[i] = cfg.toAmbient[i];
        t_[i] = ambientK;
        for (std::size_t j = 0; j < n; ++j) inv_[i][j] = m[i][n + j];
    }
    return true;
}

void ThermalNetwork::setState(const double* temperaturesK) noexcept {
    for (std::size_t i = 0; i < n_; ++i) t_[i] = temperaturesK[i];
}

void ThermalNetwork::step(const double* powerW) noexcept {
    std::array<double, kMaxNodes> rhs;
    for (std::size_t i = 0; i < n_; ++i) rhs[i] = cOverDt_[i] * t_[i] + powerW[i] + gAmb_[i] * ambient_;
    for (std::size_t i = 0; i < n_; ++i) {
        double s = 0.0;
        for (std::size_t j = 0; j < n_; ++j) s += inv_[i][j] * rhs[j];
        t_[i] = s;
    }
}

}  // namespace cd::dsp
