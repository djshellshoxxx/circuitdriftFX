#include "dsp/MnaSolver.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace cd::dsp {
namespace {
int findRoot(std::vector<int>& p, int i) {
    while (p[i] != i) i = p[i] = p[p[i]];
    return i;
}
}  // namespace

bool MnaSolver::prepare(const Netlist& net, double h, const Options& opt, std::string& err) {
    MnaSolver prepared;
    if (!prepared.prepareInternal(net, h, opt, err)) return false;
    *this = std::move(prepared);
    return true;
}

bool MnaSolver::prepareInternal(const Netlist& net, double h, const Options& opt, std::string& err) {
    auto bad = [&](const char* why) { err = why; m_ = 0; return false; };
    if (!(h > 0 && std::isfinite(h))) return bad("substep h must be positive and finite");
    if (opt.maxNewton < 1 || opt.maxNewton > 12 || !(opt.tol > 0 && opt.tol <= 1e-7 && std::isfinite(opt.tol)))
        return bad("Newton options out of range (finite 0 < tolerance <= 1e-7 required)");
    if (net.nodes < 1 || net.nodes > kMaxNodes) return bad("node count outside 1..32");
    if (net.elements.empty() || net.elements.size() > std::size_t(kMaxElements)) return bad("element count outside 1..68 (64 components + 4 port sources)");
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
            std::size_t paramCount = 0;
            switch (e.model->equation) {
                case DeviceEquation::Resistor: case DeviceEquation::Capacitor: paramCount = 1; break;
                case DeviceEquation::ShockleyDiode: paramCount = 3; break;
                default: return bad("unsupported device equation");
            }
            if (e.model->params.size() != paramCount) return bad("device parameter count does not match equation");
            for (std::size_t k = 0; k < e.model->params.size(); ++k) {
                const auto& p = e.model->params[k];
                if (!(std::isfinite(p.min) && std::isfinite(p.max) && p.min > 0 &&
                      std::isfinite(e.param[k]) && e.param[k] >= p.min && e.param[k] <= p.max))
                    return bad("device parameter outside finite positive manifest bounds");
            }
            if (e.model->equation == DeviceEquation::Resistor && !std::isfinite(1.0 / e.param[0]))
                return bad("resistor conductance exceeds numeric range");
            if (e.model->equation == DeviceEquation::Capacitor && !std::isfinite(e.param[0] / h))
                return bad("capacitor step conductance exceeds numeric range");
            if (e.model->equation == DeviceEquation::ShockleyDiode &&
                (!(e.model->maxExpArg > 0 && e.model->maxExpArg <= 80 && std::isfinite(e.model->maxExpArg)) ||
                 !(e.param[1] * e.param[2] > 0 && std::isfinite(e.param[1] * e.param[2]))))
                return bad("diode domain exceeds numeric range");
            conductive = e.model->equation != DeviceEquation::Capacitor;
            if (e.model->equation == DeviceEquation::ShockleyDiode && ++nonlinear > kMaxNonlinear) return bad("more than 8 nonlinear devices");
        } else {
            if (e.kind != NetElement::Kind::VoltageSource && e.kind != NetElement::Kind::CurrentSource)
                return bad("unsupported source kind");
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
    const std::size_t pad = std::size_t(m_) + 1;
    for (auto* v : {&x_, &xSave_, &xTry_, &F_, &Ftry_, &dx_, &rowScale_, &unitScale_, &xEval_, &FEval_}) v->assign(pad, 0.0);
    J_.assign(pad * pad, 0.0);
    Jtry_.assign(pad * pad, 0.0);
    JEval_.assign(pad * pad, 0.0);
    for (int k = 0; k < m_; ++k) unitScale_[k] = k < n_ ? vScale_ : iScale_;
    buildOps();
    failures_ = 0; consecutive_ = 0;
    if (!newton(kDcIterations, true)) return bad("DC operating point did not converge or matrix singular");
    if (!finiteSolution(x_.data())) return bad("DC state or capacitor charge exceeds numeric range");
    storeCharges();
    err.clear();
    return true;
}

void MnaSolver::setSource(int k, double value) noexcept {
    if (k >= 0 && k < int(el_.size()) && el_[k].kind != NetElement::Kind::Device) el_[k].value = value;
}

bool MnaSolver::setElementParameter(int k, int pi, double value) noexcept {
    if (k < 0 || k >= int(el_.size())) return false;
    NetElement& e = el_[std::size_t(k)];
    if (e.kind != NetElement::Kind::Device || !e.model || pi < 0 || pi >= int(e.model->params.size()) || pi >= 3) return false;
    const ManifestParam& p = e.model->params[std::size_t(pi)];
    if (!(std::isfinite(value) && value >= p.min && value <= p.max && value > 0)) return false;
    switch (e.model->equation) {
        case DeviceEquation::Resistor:
            if (!std::isfinite(1.0 / value)) return false;
            break;
        case DeviceEquation::Capacitor:
            if (!std::isfinite(value / h_)) return false;
            break;
        case DeviceEquation::ShockleyDiode: {
            double nvt = pi == 1 ? value * e.param[2] : pi == 2 ? e.param[1] * value : e.param[1] * e.param[2];
            if (!(nvt > 0 && std::isfinite(nvt))) return false;
            break;
        }
        default: return false;
    }
    e.param[pi] = value;
    refreshOp(k);
    return true;
}

double MnaSolver::elementParameter(int k, int pi) const noexcept {
    if (k < 0 || k >= int(el_.size())) return std::numeric_limits<double>::quiet_NaN();
    const NetElement& e = el_[std::size_t(k)];
    if (e.kind != NetElement::Kind::Device || !e.model || pi < 0 || pi >= int(e.model->params.size()) || pi >= 3)
        return std::numeric_limits<double>::quiet_NaN();
    return e.param[pi];
}

void MnaSolver::buildOps() noexcept {
    ops_.assign(el_.size(), Op{});
    for (std::size_t k = 0; k < el_.size(); ++k) {
        const NetElement& e = el_[k];
        Op& o = ops_[k];
        o.a = e.a; o.b = e.b; o.elem = int(k);
        switch (e.kind) {
            case NetElement::Kind::CurrentSource: o.kind = Op::ISrc; break;
            case NetElement::Kind::VoltageSource: o.kind = Op::VSrc; o.row = vsRow_[k] + 1; break;
            case NetElement::Kind::Device:
                switch (e.model->equation) {
                    case DeviceEquation::Resistor: o.kind = Op::Res; break;
                    case DeviceEquation::Capacitor: o.kind = Op::Cap; o.slot = capSlot_[k]; break;
                    case DeviceEquation::ShockleyDiode: o.kind = Op::Diode; o.cap = e.model->maxExpArg; break;
                }
                refreshOp(int(k));
                break;
        }
    }
}

void MnaSolver::refreshOp(int k) noexcept {
    const NetElement& e = el_[std::size_t(k)];
    Op& o = ops_[std::size_t(k)];
    switch (o.kind) {
        case Op::Res: o.p0 = 1.0 / e.param[0]; break;
        case Op::Cap: o.p0 = e.param[0]; o.c1 = e.param[0] / h_; break;
        case Op::Diode: o.p0 = e.param[0]; o.c1 = e.param[1] * e.param[2]; break;
        default: break;
    }
}

// Same stamps, same order and same rounding as the original per-element loop; ground contributions
// land in padded slot 0 instead of being branched away. dc omits capacitor terms.
void MnaSolver::evalPadded(const double* x, double* F, double* J, bool dc) const noexcept {
    const std::size_t ld = std::size_t(m_) + 1;
    std::fill(F, F + ld, 0.0);
    if (J) std::fill(J, J + ld * ld, 0.0);
    for (std::size_t k = 0; k < ops_.size(); ++k) {
        const Op& o = ops_[k];
        const std::size_t a = std::size_t(o.a), b = std::size_t(o.b);
        const double v = x[a] - x[b];
        double i = 0, g = 0;
        switch (o.kind) {
            case Op::Res:
                g = o.p0; i = g * v;
                break;
            case Op::Cap:
                if (dc) continue;
                g = o.c1; i = (o.p0 * v - qOld_[std::size_t(o.slot)]) / h_;
                break;
            case Op::Diode: {
                const double arg = v / o.c1;
                if (arg > o.cap) {  // C1 linear extension beyond the documented exponent domain
                    const double ec = std::exp(o.cap);
                    i = o.p0 * (ec * (1.0 + arg - o.cap) - 1.0); g = o.p0 * ec / o.c1;
                } else {
                    const double ex = std::exp(arg);
                    i = o.p0 * (ex - 1.0); g = o.p0 * ex / o.c1;
                }
                break;
            }
            case Op::ISrc: {
                const double val = el_[std::size_t(o.elem)].value;
                F[a] += val; F[b] += -val;
                continue;
            }
            case Op::VSrc: {
                const std::size_t r = std::size_t(o.row);
                F[a] += x[r]; F[b] += -x[r];
                if (J) { J[a * ld + r] += 1.0; J[b * ld + r] += -1.0; }
                F[r] = v - el_[std::size_t(o.elem)].value;
                if (J) { J[r * ld + a] += 1.0; J[r * ld + b] += -1.0; }
                continue;
            }
        }
        F[a] += i; F[b] += -i;
        if (J) { J[a * ld + a] += g; J[a * ld + b] += -g; J[b * ld + a] += -g; J[b * ld + b] += g; }
    }
}

void MnaSolver::evaluate(const double* x, double* F, double* J, bool dc) noexcept {
    const std::size_t m = std::size_t(m_), ld = m + 1;
    for (std::size_t k = 0; k < m; ++k) xEval_[k + 1] = x[k];
    evalPadded(xEval_.data(), FEval_.data(), J ? JEval_.data() : nullptr, dc);
    for (std::size_t k = 0; k < m; ++k) F[k] = FEval_[k + 1];
    if (J)
        for (std::size_t r = 0; r < m; ++r)
            for (std::size_t c = 0; c < m; ++c) J[r * m + c] = JEval_[(r + 1) * ld + c + 1];
}

double MnaSolver::scaledResidual(const double* F) const noexcept {
    double r = 0;
    for (int k = 0; k < m_; ++k) {
        const double s = std::fabs(F[k + 1]) / (k < n_ ? iScale_ : vScale_);
        if (!std::isfinite(s)) return INFINITY;
        r = std::max(r, s);
    }
    return r;
}

// A is the (m x m) block at A[0], row stride ldi. Failure conditions match the original per-entry
// checks: any non-finite input, intermediate or output entry returns false (non-finiteness is
// sticky, so testing a row or a sum once is equivalent to testing every element).
namespace {
// M > 0 fixes the system size at compile time so small circuits fully unroll; M == 0 is the
// runtime-size fallback. The arithmetic is identical for every M.
template <int M>
bool luImpl(double* A, std::size_t ld, double* b, double* rowScale, int mRuntime) noexcept {
    const int m = M > 0 ? M : mRuntime;
    for (int r = 0; r < m; ++r) {
        if (!std::isfinite(b[r])) return false;
        const double* row = A + std::size_t(r) * ld;
        double s = 0, chk = 0;
        for (int c = 0; c < m; ++c) {
            const double a = row[c];
            chk += a - a;                       // 0 for finite, NaN otherwise
            s = std::max(s, std::fabs(a));
        }
        if (!(chk == 0.0)) return false;
        if (!(s > 0) || !std::isfinite(s)) return false;
        rowScale[r] = s;
    }
    for (int k = 0; k < m; ++k) {
        int p = k;
        double best = -1;
        for (int r = k; r < m; ++r) {
            const double q = std::fabs(A[std::size_t(r) * ld + k]) / rowScale[r];
            if (!std::isfinite(q)) return false;
            if (q > best) { best = q; p = r; }
        }
        if (!(best > 1e-13)) return false;  // singular relative to the row's scale
        if (p != k) {
            // Columns < k of rows >= k are never read again, so only c >= k need to move.
            for (int c = k; c < m; ++c) std::swap(A[std::size_t(k) * ld + c], A[std::size_t(p) * ld + c]);
            std::swap(b[k], b[p]);
            std::swap(rowScale[k], rowScale[p]);
        }
        const double* rk = A + std::size_t(k) * ld;
        const double piv = rk[k];
        if (!std::isfinite(piv) || piv == 0) return false;
        for (int r = k + 1; r < m; ++r) {
            double* rr = A + std::size_t(r) * ld;
            const double f = rr[k] / piv;
            if (!std::isfinite(f)) return false;
            if (f == 0) continue;
            double chk = 0;
            for (int c = k; c < m; ++c) {
                const double a = rr[c] - f * rk[c];
                rr[c] = a;
                chk += a - a;
            }
            if (!(chk == 0.0)) return false;
            b[r] -= f * b[k];
            if (!std::isfinite(b[r])) return false;
        }
    }
    for (int k = m - 1; k >= 0; --k) {
        const double* rk = A + std::size_t(k) * ld;
        double s = b[k];
        for (int c = k + 1; c < m; ++c) s -= rk[c] * b[c];
        if (!std::isfinite(s)) return false;
        b[k] = s / rk[k];
        if (!std::isfinite(b[k])) return false;
    }
    return true;
}
}  // namespace

bool MnaSolver::luSolve(double* A, int ldi, double* b) noexcept {
    const std::size_t ld = std::size_t(ldi);
    double* rs = rowScale_.data();
    switch (m_) {
        case 1: return luImpl<1>(A, ld, b, rs, 1);
        case 2: return luImpl<2>(A, ld, b, rs, 2);
        case 3: return luImpl<3>(A, ld, b, rs, 3);
        case 4: return luImpl<4>(A, ld, b, rs, 4);
        case 5: return luImpl<5>(A, ld, b, rs, 5);
        case 6: return luImpl<6>(A, ld, b, rs, 6);
        case 7: return luImpl<7>(A, ld, b, rs, 7);
        case 8: return luImpl<8>(A, ld, b, rs, 8);
        default: return luImpl<0>(A, ld, b, rs, m_);
    }
}

bool MnaSolver::newton(int maxIter, bool dc) noexcept {
    const int m = m_, ld = m_ + 1;
    const double* unit = unitScale_.data();
    double* dx = dx_.data();
    bool haveEval = false;   // F_/J_ already hold the evaluation at x_ (from the accepted trial)
    double res0 = 0;
    for (int it = 1; it <= maxIter; ++it) {
        if (haveEval) {
            // The accepted trial evaluated exactly (x_, same step context): reuse it bit-for-bit.
            std::swap(F_, Ftry_);
            std::swap(J_, Jtry_);
        } else {
            evalPadded(x_.data(), F_.data(), J_.data(), dc);
            res0 = scaledResidual(F_.data());
        }
        if (!std::isfinite(res0)) return false;
        const double* F = F_.data();
        const double* x = x_.data();
        for (int k = 0; k < m; ++k) dx[k] = -F[k + 1];
        if (!luSolve(J_.data() + ld + 1, ld, dx)) return false;
        double stepRel = 0;
        for (int k = 0; k < m; ++k) {
            // Compute |dx|/(|x|+unit) without overflowing the denominator.
            const double xk = x[k + 1];
            const double scale = std::max(std::fabs(xk), unit[k]);
            const double rel = (std::fabs(dx[k]) / scale) / (std::fabs(xk) / scale + unit[k] / scale);
            if (!std::isfinite(rel)) return false;
            stepRel = std::max(stepRel, rel);
        }
        double* xt = xTry_.data();
        if (res0 <= tol_ && stepRel <= tol_) {
            // Retain the final correction, but validate the state and its actual residual.
            for (int k = 1; k <= m; ++k) xt[k] = x[k] + dx[k - 1];
            if (!finiteSolution(xt)) return false;
            evalPadded(xt, Ftry_.data(), nullptr, dc);
            const double res = scaledResidual(Ftry_.data());
            if (!std::isfinite(res) || res > tol_) return false;
            std::swap(x_, xTry_);
            lastIter_ = it; lastRes_ = res;
            return true;
        }
        double lambda = 1.0, res = INFINITY;
        for (int trial = 0; trial < 4; ++trial, lambda *= 0.5) {
            for (int k = 1; k <= m; ++k) xt[k] = x[k] + lambda * dx[k - 1];
            res = INFINITY;
            if (finiteSolution(xt)) {
                evalPadded(xt, Ftry_.data(), Jtry_.data(), dc);
                res = scaledResidual(Ftry_.data());
            }
            if (res < res0 || (trial == 3 && std::isfinite(res))) break;
        }
        if (!std::isfinite(res)) return false;
        std::swap(x_, xTry_);
        if (res <= tol_ && lambda * stepRel <= tol_) { lastIter_ = it; lastRes_ = res; return true; }
        haveEval = true;
        res0 = res;
    }
    lastIter_ = maxIter;
    return false;
}

bool MnaSolver::step() noexcept {
    if (m_ == 0) return false;
    xSave_ = x_;  // same size: element copy, no allocation
    // newton() returns true only for a state it has already validated with finiteSolution().
    if (!newton(maxNewton_, false)) {
        x_ = xSave_;
        if (failures_ < std::numeric_limits<std::int64_t>::max()) ++failures_;
        if (consecutive_ < 3) ++consecutive_;
        return false;
    }
    storeCharges();
    consecutive_ = 0;
    return true;
}

void MnaSolver::getState(double* out) const noexcept {
    std::copy(x_.begin() + 1, x_.end(), out);
    std::copy(qOld_.begin(), qOld_.end(), out + m_);
}

void MnaSolver::setState(const double* in) noexcept {
    if (!in) return;
    for (int k = 0; k < m_; ++k) xEval_[std::size_t(k) + 1] = in[k];
    if (!finiteSolution(xEval_.data())) return;
    for (std::size_t k = 0; k < qOld_.size(); ++k)
        if (!std::isfinite(in[m_ + k])) return;
    std::copy(in, in + m_, x_.begin() + 1);
    std::copy(in + m_, in + m_ + qOld_.size(), qOld_.begin());
}

bool MnaSolver::finiteSolution(const double* x) const noexcept {
    // x - x is 0 for finite values and NaN otherwise; one sticky test replaces per-value branches.
    double chk = 0;
    for (int k = 1; k <= m_; ++k) chk += x[k] - x[k];
    for (const Op& o : ops_) {
        const double v = x[o.a] - x[o.b];
        chk += v - v;
        if (o.kind == Op::Cap) { const double q = o.p0 * v; chk += q - q; }
    }
    return chk == 0.0;
}

void MnaSolver::storeCharges() noexcept {
    // finiteSolution checks every charge before any is committed.
    for (const Op& o : ops_)
        if (o.kind == Op::Cap) qOld_[std::size_t(o.slot)] = o.p0 * (x_[std::size_t(o.a)] - x_[std::size_t(o.b)]);
}

}  // namespace cd::dsp
