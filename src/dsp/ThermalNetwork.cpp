#include "ThermalNetwork.h"
#include <cmath>
#include <limits>
#include <utility>

namespace cd::dsp {

bool ThermalNetwork::prepare(const Config& cfg, double dt, double ambientK) noexcept {
    const std::size_t n = cfg.n;
    if (n == 0 || n > kMaxNodes || !(dt > 0.0) || !std::isfinite(dt) ||
        !(ambientK > 0.0) || !std::isfinite(ambientK)) return false;
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
    std::array<double, kMaxNodes> cOverDt{};
    for (std::size_t i = 0; i < n; ++i) {
        cOverDt[i] = cfg.capacity[i] / dt;
        if (!(cOverDt[i] > 0.0) || !std::isfinite(cOverDt[i])) return false;
        // The initial zero-power step must also be numerically representable.
        const double ambientInput = cfg.toAmbient[i] * ambientK;
        if (!std::isfinite(ambientInput) || !std::isfinite(cOverDt[i] * ambientK + ambientInput)) return false;
        double diag = cOverDt[i] + cfg.toAmbient[i];
        if (!std::isfinite(diag)) return false;
        for (std::size_t j = 0; j < n; ++j) {
            diag += cfg.coupling[i][j];
            if (!std::isfinite(diag)) return false;
            m[i][j] = -cfg.coupling[i][j];
        }
        m[i][i] = diag;
        m[i][n + i] = 1.0;
    }
    for (std::size_t c = 0; c < n; ++c) {
        std::size_t piv = c;
        for (std::size_t r = c + 1; r < n; ++r)
            if (std::fabs(m[r][c]) > std::fabs(m[piv][c])) piv = r;
        if (!(std::fabs(m[piv][c]) > 1e-300) || !std::isfinite(m[piv][c])) return false;
        std::swap(m[c], m[piv]);
        const double d = 1.0 / m[c][c];
        if (!std::isfinite(d)) return false;
        for (std::size_t k = 0; k < 2 * n; ++k) {
            m[c][k] *= d;
            if (!std::isfinite(m[c][k])) return false;
        }
        for (std::size_t r = 0; r < n; ++r) {
            if (r == c) continue;
            const double f = m[r][c];
            if (f == 0.0) continue;
            for (std::size_t k = 0; k < 2 * n; ++k) {
                m[r][k] -= f * m[c][k];
                if (!std::isfinite(m[r][k])) return false;
            }
        }
    }
    n_ = n;
    ambient_ = ambientK;
    clearStepDiagnostics();
    for (std::size_t i = 0; i < n; ++i) {
        cOverDt_[i] = cOverDt[i];
        gAmb_[i] = cfg.toAmbient[i];
        t_[i] = ambientK;
        for (std::size_t j = 0; j < n; ++j) inv_[i][j] = m[i][n + j];
    }
    return true;
}

void ThermalNetwork::setState(const double* temperaturesK) noexcept {
    if (!temperaturesK) return;
    for (std::size_t i = 0; i < n_; ++i) {
        if (!(temperaturesK[i] > 0.0) || !std::isfinite(temperaturesK[i]) ||
            !std::isfinite(cOverDt_[i] * temperaturesK[i] + gAmb_[i] * ambient_)) return;
    }
    for (std::size_t i = 0; i < n_; ++i) t_[i] = temperaturesK[i];
}

void ThermalNetwork::clearStepDiagnostics() noexcept {
    lastStatus_ = StepStatus::None;
    accepted_ = rejected_ = consecutiveRejected_ = 0;
}

void ThermalNetwork::step(const double* powerW) noexcept {
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    lastStatus_ = integrate(powerW);
    if (lastStatus_ == StepStatus::Accepted) {
        if (accepted_ != kMax) ++accepted_;
        consecutiveRejected_ = 0;
    } else {
        if (rejected_ != kMax) ++rejected_;
        if (consecutiveRejected_ != kMax) ++consecutiveRejected_;
    }
}

// Computes the full candidate before committing; any early return leaves t_ untouched.
ThermalNetwork::StepStatus ThermalNetwork::integrate(const double* powerW) noexcept {
    if (n_ == 0) return StepStatus::NotPrepared;
    if (!powerW) return StepStatus::NullInput;
    std::array<double, kMaxNodes> rhs{}, candidate{};
    for (std::size_t i = 0; i < n_; ++i) {
        if (!std::isfinite(powerW[i])) return StepStatus::NonFinitePower;
        rhs[i] = cOverDt_[i] * t_[i] + powerW[i] + gAmb_[i] * ambient_;
        if (!std::isfinite(rhs[i])) return StepStatus::Overflow;
    }
    for (std::size_t i = 0; i < n_; ++i) {
        double s = 0.0;
        for (std::size_t j = 0; j < n_; ++j) {
            s += inv_[i][j] * rhs[j];
            if (!std::isfinite(s)) return StepStatus::Overflow;
        }
        if (!(s > 0.0)) return StepStatus::NonPositiveTemperature;
        if (!std::isfinite(cOverDt_[i] * s + gAmb_[i] * ambient_)) return StepStatus::Overflow;  // next step unrepresentable
        candidate[i] = s;
    }
    for (std::size_t i = 0; i < n_; ++i) t_[i] = candidate[i];
    return StepStatus::Accepted;
}

}  // namespace cd::dsp
