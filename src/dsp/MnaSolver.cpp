#include "dsp/MnaSolver.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace cd::dsp {
namespace {
int findRoot(std::vector<int>& p, int i) {
    while (p[i] != i) i = p[i] = p[p[i]];
    return i;
}
}  // namespace

bool MnaSolver::prepare(const Netlist& net, double h, const Options& opt, std::string& err) {
    auto bad = [&](const char* why) { err = why; m_ = 0; return false; };
    if (!(h > 0 && std::isfinite(h))) return bad("substep h must be positive and finite");
    if (opt.maxNewton < 1 || opt.maxNewton > 12 || !(opt.tol > 0)) return bad("Newton options out of range");
    if (net.nodes < 1 || net.nodes > kMaxNodes) return bad("node count outside 1..32");
    if (net.elements.empty() || int(net.elements.size()) > kMaxComponents) return bad("component count outside 1..64");
    if (!(net.voltageScaleV > 0 && net.currentScaleA > 0 && std::isfinite(net.voltageScaleV) && std::isfinite(net.currentScaleA)))
        return bad("circuit scales must be strictly positive");

    int vs = 0, nonlinear = 0;
    std::vector<int> dcSet(net.nodes + 1), vsSet(net.nodes + 1);
    std::iota(dcSet.begin(), dcSet.end(), 0);
    std::iota(vsSet.begin(), vsSet.end(), 0);
    for (const auto& e : net.elements) {
        if (e.a < 0 || e.b < 0 || e.a > net.nodes || e.b > net.nodes || e.a == e.b) return bad("element node index invalid");
        bool conductive = false;
        if (e.kind == NetElement::Kind::Device) {
            if (!e.model || e.model->pins.size() != 2 || e.model->params.size() > 3) return bad("device without supported manifest");
            for (std::size_t k = 0; k < e.model->params.size(); ++k) {
                const auto& p = e.model->params[k];
                if (!(e.param[k] >= p.min && e.param[k] <= p.max)) return bad("device parameter outside manifest bounds");
            }
            conductive = e.model->equation != DeviceEquation::Capacitor;
            if (e.model->equation == DeviceEquation::ShockleyDiode && ++nonlinear > kMaxNonlinear) return bad("more than 8 nonlinear devices");
        } else {
            if (!std::isfinite(e.value)) return bad("non-finite source value");
            if (e.kind == NetElement::Kind::VoltageSource) {
                ++vs;
                conductive = true;
                const int ra = findRoot(vsSet, e.a), rb = findRoot(vsSet, e.b);
                if (ra == rb) return bad("ideal voltage-source loop");
                vsSet[ra] = rb;
            }
        }
        if (conductive) dcSet[findRoot(dcSet, e.a)] = findRoot(dcSet, e.b);
    }
    for (int i = 1; i <= net.nodes; ++i)
        if (findRoot(dcSet, i) != findRoot(dcSet, 0)) return bad("node without DC path to ground (floating)");
    if (net.nodes + vs > kMaxUnknowns) return bad("more than 96 unknowns");

    el_ = net.elements;
    n_ = net.nodes;
    m_ = n_ + vs;
    h_ = h; tol_ = opt.tol; maxNewton_ = opt.maxNewton;
    vScale_ = net.voltageScaleV; iScale_ = net.currentScaleA;
    vsRow_.assign(el_.size(), -1);
    capSlot_.assign(el_.size(), -1);
    int row = n_, caps = 0;
    for (std::size_t k = 0; k < el_.size(); ++k) {
        if (el_[k].kind == NetElement::Kind::VoltageSource) vsRow_[k] = row++;
        else if (el_[k].kind == NetElement::Kind::Device && el_[k].model->equation == DeviceEquation::Capacitor) capSlot_[k] = caps++;
    }
    qOld_.assign(caps, 0.0);
    for (auto* v : {&x_, &xSave_, &xTry_, &F_, &Ftry_, &dx_, &rowScale_, &unitScale_}) v->assign(m_, 0.0);
    J_.assign(std::size_t(m_) * m_, 0.0);
    for (int k = 0; k < m_; ++k) unitScale_[k] = k < n_ ? vScale_ : iScale_;
    failures_ = 0; consecutive_ = 0;
    if (!newton(kDcIterations, true)) return bad("DC operating point did not converge or matrix singular");
    for (std::size_t k = 0; k < el_.size(); ++k)
        if (capSlot_[k] >= 0) qOld_[capSlot_[k]] = el_[k].param[0] * (voltage(el_[k].a) - voltage(el_[k].b));
    err.clear();
    return true;
}

void MnaSolver::setSource(int k, double value) noexcept {
    if (k >= 0 && k < int(el_.size()) && el_[k].kind != NetElement::Kind::Device) el_[k].value = value;
}

void MnaSolver::evaluate(const double* x, double* F, double* J, bool dc) noexcept {
    std::fill(F, F + m_, 0.0);
    if (J) std::fill(J, J + std::size_t(m_) * m_, 0.0);
    auto V = [&](int node) { return node == 0 ? 0.0 : x[node - 1]; };
    auto addF = [&](int node, double i) { if (node) F[node - 1] += i; };
    auto addJ = [&](int r, int c, double g) { if (J && r >= 0 && c >= 0) J[std::size_t(r) * m_ + c] += g; };
    auto stamp = [&](int a, int b, double g) {  // conductance-like 2-terminal Jacobian
        addJ(a - 1, a - 1, g); addJ(a - 1, b - 1, -g); addJ(b - 1, a - 1, -g); addJ(b - 1, b - 1, g);
    };
    for (std::size_t k = 0; k < el_.size(); ++k) {
        const NetElement& e = el_[k];
        const double v = V(e.a) - V(e.b);
        double i = 0, g = 0;
        switch (e.kind) {
            case NetElement::Kind::Device:
                switch (e.model->equation) {
                    case DeviceEquation::Resistor:
                        g = 1.0 / e.param[0]; i = g * v;
                        break;
                    case DeviceEquation::Capacitor:
                        if (dc) continue;
                        g = e.param[0] / h_; i = (e.param[0] * v - qOld_[capSlot_[k]]) / h_;
                        break;
                    case DeviceEquation::ShockleyDiode: {
                        const double is = e.param[0], nvt = e.param[1] * e.param[2], cap = e.model->maxExpArg;
                        const double arg = v / nvt;
                        if (arg > cap) {  // C1 linear extension beyond the documented exponent domain
                            const double ec = std::exp(cap);
                            i = is * (ec * (1.0 + arg - cap) - 1.0); g = is * ec / nvt;
                        } else {
                            const double ex = std::exp(arg);
                            i = is * (ex - 1.0); g = is * ex / nvt;
                        }
                        break;
                    }
                }
                addF(e.a, i); addF(e.b, -i);
                stamp(e.a, e.b, g);
                break;
            case NetElement::Kind::CurrentSource:
                addF(e.a, e.value); addF(e.b, -e.value);
                break;
            case NetElement::Kind::VoltageSource: {
                const int r = vsRow_[k];
                addF(e.a, x[r]); addF(e.b, -x[r]);
                addJ(e.a - 1, r, 1.0); addJ(e.b - 1, r, -1.0);
                F[r] = v - e.value;
                addJ(r, e.a - 1, 1.0); addJ(r, e.b - 1, -1.0);
                break;
            }
        }
    }
}

double MnaSolver::scaledResidual(const double* F) const noexcept {
    double r = 0;
    for (int k = 0; k < m_; ++k) {
        const double s = std::fabs(F[k]) / (k < n_ ? iScale_ : vScale_);
        if (!(s == s)) return INFINITY;
        r = std::max(r, s);
    }
    return r;
}

bool MnaSolver::luSolve(double* A, double* b) noexcept {
    const int m = m_;
    for (int r = 0; r < m; ++r) {
        double s = 0;
        for (int c = 0; c < m; ++c) s = std::max(s, std::fabs(A[std::size_t(r) * m + c]));
        if (!(s > 0) || !std::isfinite(s)) return false;
        rowScale_[r] = s;
    }
    for (int k = 0; k < m; ++k) {
        int p = k;
        double best = -1;
        for (int r = k; r < m; ++r) {
            const double q = std::fabs(A[std::size_t(r) * m + k]) / rowScale_[r];
            if (q > best) { best = q; p = r; }
        }
        if (!(best > 1e-13)) return false;  // singular relative to the row's scale
        if (p != k) {
            for (int c = 0; c < m; ++c) std::swap(A[std::size_t(k) * m + c], A[std::size_t(p) * m + c]);
            std::swap(b[k], b[p]);
            std::swap(rowScale_[k], rowScale_[p]);
        }
        const double piv = A[std::size_t(k) * m + k];
        for (int r = k + 1; r < m; ++r) {
            const double f = A[std::size_t(r) * m + k] / piv;
            if (f == 0) continue;
            for (int c = k; c < m; ++c) A[std::size_t(r) * m + c] -= f * A[std::size_t(k) * m + c];
            b[r] -= f * b[k];
        }
    }
    for (int k = m - 1; k >= 0; --k) {
        double s = b[k];
        for (int c = k + 1; c < m; ++c) s -= A[std::size_t(k) * m + c] * b[c];
        b[k] = s / A[std::size_t(k) * m + k];
    }
    return true;
}

bool MnaSolver::newton(int maxIter, bool dc) noexcept {
    for (int it = 1; it <= maxIter; ++it) {
        evaluate(x_.data(), F_.data(), J_.data(), dc);
        const double res0 = scaledResidual(F_.data());
        for (int k = 0; k < m_; ++k) dx_[k] = -F_[k];
        if (!luSolve(J_.data(), dx_.data())) return false;
        double stepRel = 0;
        for (int k = 0; k < m_; ++k) stepRel = std::max(stepRel, std::fabs(dx_[k]) / (std::fabs(x_[k]) + unitScale_[k]));
        if (!(stepRel == stepRel)) return false;
        if (res0 <= tol_ && stepRel <= tol_) {  // converged; the already-solved step is applied for free
            for (int k = 0; k < m_; ++k) x_[k] += dx_[k];
            lastIter_ = it; lastRes_ = res0;
            return true;
        }
        double lambda = 1.0, res = INFINITY;
        for (int trial = 0; trial < 4; ++trial, lambda *= 0.5) {
            for (int k = 0; k < m_; ++k) xTry_[k] = x_[k] + lambda * dx_[k];
            evaluate(xTry_.data(), Ftry_.data(), nullptr, dc);
            res = scaledResidual(Ftry_.data());
            if (res < res0 || (trial == 3 && std::isfinite(res))) break;
        }
        if (!std::isfinite(res)) return false;
        std::swap(x_, xTry_);
        if (res <= tol_ && lambda * stepRel <= tol_) { lastIter_ = it; lastRes_ = res; return true; }
    }
    lastIter_ = maxIter;
    return false;
}

bool MnaSolver::step() noexcept {
    if (m_ == 0) return false;
    xSave_ = x_;  // same size: element copy, no allocation
    if (!newton(maxNewton_, false)) {
        x_ = xSave_;
        ++failures_; ++consecutive_;
        return false;
    }
    for (std::size_t k = 0; k < el_.size(); ++k)
        if (capSlot_[k] >= 0) qOld_[capSlot_[k]] = el_[k].param[0] * (voltage(el_[k].a) - voltage(el_[k].b));
    consecutive_ = 0;
    return true;
}

void MnaSolver::getState(double* out) const noexcept {
    std::copy(x_.begin(), x_.end(), out);
    std::copy(qOld_.begin(), qOld_.end(), out + m_);
}

void MnaSolver::setState(const double* in) noexcept {
    std::copy(in, in + m_, x_.begin());
    std::copy(in + m_, in + m_ + qOld_.size(), qOld_.begin());
}

}  // namespace cd::dsp
