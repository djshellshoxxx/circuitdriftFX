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
    for (auto* v : {&x_, &xSave_, &xTry_, &F_, &Ftry_, &dx_, &rowScale_, &unitScale_}) v->assign(m_, 0.0);
    J_.assign(std::size_t(m_) * m_, 0.0);
    st_.assign(el_.size(), Stamp{});
    for (std::size_t k = 0; k < el_.size(); ++k) compile(int(k));
    refreshCapMax();
    for (int k = 0; k < m_; ++k) unitScale_[k] = k < n_ ? vScale_ : iScale_;
    failures_ = 0; consecutive_ = 0;
    if (!newton(kDcIterations, true)) return bad("DC operating point did not converge or matrix singular");
    if (!finiteSolution(x_.data())) return bad("DC state or capacitor charge exceeds numeric range");
    storeCharges();
    err.clear();
    return true;
}

void MnaSolver::compile(int k) noexcept {
    const NetElement& e = el_[std::size_t(k)];
    Stamp& s = st_[std::size_t(k)];
    s.ia = e.a - 1; s.ib = e.b - 1;
    s.value = e.value;
    switch (e.kind) {
        case NetElement::Kind::CurrentSource: s.kind = Stamp::CurrentSource; return;
        case NetElement::Kind::VoltageSource: s.kind = Stamp::VoltageSource; s.aux = vsRow_[std::size_t(k)]; return;
        case NetElement::Kind::Device: break;
    }
    s.p0 = e.param[0];
    switch (e.model->equation) {
        case DeviceEquation::Resistor: s.kind = Stamp::Resistor; s.g = 1.0 / e.param[0]; break;
        case DeviceEquation::Capacitor: s.kind = Stamp::Capacitor; s.g = e.param[0] / h_; s.aux = capSlot_[std::size_t(k)]; break;
        case DeviceEquation::ShockleyDiode:
            s.kind = Stamp::Diode;
            s.nvt = e.param[1] * e.param[2];
            s.cap = e.model->maxExpArg;
            s.expCap = std::exp(s.cap);
            s.lastArg = std::numeric_limits<double>::quiet_NaN();
            s.lastExp = 0;
            break;
        default: break;  // rejected by prepare
    }
}

void MnaSolver::refreshCapMax() noexcept {
    capMax_ = 0;
    for (const Stamp& s : st_)
        if (s.kind == Stamp::Capacitor) capMax_ = std::max(capMax_, s.p0);
}

void MnaSolver::setSource(int k, double value) noexcept {
    if (k >= 0 && k < int(el_.size()) && el_[std::size_t(k)].kind != NetElement::Kind::Device) {
        el_[std::size_t(k)].value = value;
        st_[std::size_t(k)].value = value;
    }
}

bool MnaSolver::updateParameter(int element, int k, double value) noexcept {
    if (m_ == 0 || element < 0 || element >= int(el_.size())) return false;
    const NetElement& e = el_[std::size_t(element)];
    if (e.kind != NetElement::Kind::Device || !e.model || k < 0 || k >= int(e.model->params.size())) return false;
    const ManifestParam& p = e.model->params[std::size_t(k)];
    if (!std::isfinite(value) || !(value >= p.min && value <= p.max)) return false;
    switch (e.model->equation) {
        case DeviceEquation::Resistor:
            if (!std::isfinite(1.0 / value)) return false;
            break;
        case DeviceEquation::Capacitor: {
            if (!std::isfinite(value / h_)) return false;
            const double v = voltage(e.a) - voltage(e.b);
            if (!std::isfinite(value * v)) return false;  // finiteSolution would reject the state
            break;
        }
        case DeviceEquation::ShockleyDiode: {
            const double nvt = (k == 1 ? value : e.param[1]) * (k == 2 ? value : e.param[2]);
            if (!(nvt > 0 && std::isfinite(nvt))) return false;
            break;
        }
        default: return false;
    }
    el_[std::size_t(element)].param[k] = value;
    compile(element);
    refreshCapMax();
    return true;
}

double MnaSolver::parameter(int element, int k) const noexcept {
    if (element < 0 || element >= int(el_.size())) return std::numeric_limits<double>::quiet_NaN();
    const NetElement& e = el_[std::size_t(element)];
    if (e.kind != NetElement::Kind::Device || !e.model || k < 0 || k >= int(e.model->params.size()))
        return std::numeric_limits<double>::quiet_NaN();
    return e.param[k];
}

// Same element order and the same floating-point operations as the generic stamping rules, so
// F and J are bit-identical to a direct evaluation; only bookkeeping is precomputed.
// Size dispatch: M = m_ for m_ <= 8 (compile-time trip counts), M = 0 (runtime m_) otherwise.
#define CD_MNA_DISPATCH(call)                                         \
    switch (m_) {                                                     \
        case 1: { constexpr int M = 1; return call; }                 \
        case 2: { constexpr int M = 2; return call; }                 \
        case 3: { constexpr int M = 3; return call; }                 \
        case 4: { constexpr int M = 4; return call; }                 \
        case 5: { constexpr int M = 5; return call; }                 \
        case 6: { constexpr int M = 6; return call; }                 \
        case 7: { constexpr int M = 7; return call; }                 \
        case 8: { constexpr int M = 8; return call; }                 \
        default: { constexpr int M = 0; return call; }                \
    }

void MnaSolver::evaluate(const double* x, double* F, double* J, bool dc) noexcept {
    CD_MNA_DISPATCH(evalT<M>(x, F, J, dc))
}

template <int M>
void MnaSolver::evalT(const double* x, double* F, double* J, bool dc) noexcept {
    const int m = M > 0 ? M : m_;
    for (int k = 0; k < m; ++k) F[k] = 0.0;
    if (J) for (int k = 0, mm = m * m; k < mm; ++k) J[k] = 0.0;
    const double* q = qOld_.data();
    const double h = h_;
    Stamp* const st = st_.data();
    const int count = int(st_.size());
    for (int k = 0; k < count; ++k) {
        Stamp& s = st[k];
        const int ia = s.ia, ib = s.ib;
        const double v = (ia >= 0 ? x[ia] : 0.0) - (ib >= 0 ? x[ib] : 0.0);
        double i, g;
        switch (s.kind) {
            case Stamp::Resistor:
                g = s.g; i = g * v;
                break;
            case Stamp::Capacitor:
                if (dc) continue;
                g = s.g; i = (s.p0 * v - q[s.aux]) / h;
                break;
            case Stamp::Diode: {
                const double is = s.p0, nvt = s.nvt, cap = s.cap;
                const double arg = v / nvt;
                if (arg > cap) {  // C1 linear extension beyond the documented exponent domain
                    const double ec = s.expCap;
                    i = is * (ec * (1.0 + arg - cap) - 1.0); g = is * ec / nvt;
                } else {
                    double ex;
                    if (arg == s.lastArg) {
                        ex = s.lastExp;
                    } else {
                        ex = std::exp(arg);
                        s.lastArg = arg; s.lastExp = ex;
                    }
                    i = is * (ex - 1.0); g = is * ex / nvt;
                }
                break;
            }
            case Stamp::CurrentSource:
                if (ia >= 0) F[ia] += s.value;
                if (ib >= 0) F[ib] += -s.value;
                continue;
            case Stamp::VoltageSource: {
                const int r = s.aux;
                const double xr = x[r];
                if (ia >= 0) F[ia] += xr;
                if (ib >= 0) F[ib] += -xr;
                if (J) {
                    double* Jr = J + std::size_t(r) * m;
                    if (ia >= 0) { J[std::size_t(ia) * m + r] += 1.0; }
                    if (ib >= 0) { J[std::size_t(ib) * m + r] += -1.0; }
                    F[r] = v - s.value;
                    if (ia >= 0) Jr[ia] += 1.0;
                    if (ib >= 0) Jr[ib] += -1.0;
                } else {
                    F[r] = v - s.value;
                }
                continue;
            }
            default: continue;
        }
        if (ia >= 0) F[ia] += i;
        if (ib >= 0) F[ib] += -i;
        if (J) {
            if (ia >= 0) {
                double* Ja = J + std::size_t(ia) * m;
                Ja[ia] += g;
                if (ib >= 0) Ja[ib] += -g;
            }
            if (ib >= 0) {
                double* Jb = J + std::size_t(ib) * m;
                if (ia >= 0) Jb[ia] += -g;
                Jb[ib] += g;
            }
        }
    }
}

template <int M>
double MnaSolver::resT(const double* F) const noexcept {
    const int m = M > 0 ? M : m_;
    double r = 0;
    const double is = iScale_, vs = vScale_;
    for (int k = 0; k < n_; ++k) {
        const double s = std::fabs(F[k]) / is;
        if (!std::isfinite(s)) return INFINITY;
        r = std::max(r, s);
    }
    for (int k = n_; k < m; ++k) {
        const double s = std::fabs(F[k]) / vs;
        if (!std::isfinite(s)) return INFINITY;
        r = std::max(r, s);
    }
    return r;
}

#define CD_LU_UNROLL _Pragma("GCC unroll 8")
// Scaled partial-pivot LU, row-major fixed stride. M > 0 fixes the size at compile time (fully
// unrollable for the common small circuits); M == 0 uses mRuntime. Finiteness of the eliminated
// entries is checked once at the end: a non-finite value can never become finite again under
// a -= f*b or the back substitution (pivots are checked finite and non-zero before use), and row
// swaps only move values, so a single final scan returns false exactly when the per-operation
// checks would have, while every arithmetic operation on the success path is unchanged.
template <int M>
bool MnaSolver::luFixed(double* A, double* b, int mRuntime) noexcept {
    const int m = M > 0 ? M : mRuntime;
    double* rs = rowScale_.data();
    CD_LU_UNROLL
    for (int r = 0; r < m; ++r) {
        if (!std::isfinite(b[r])) return false;
        double s = 0;
        const double* Ar = A + std::size_t(r) * m;
        CD_LU_UNROLL
        for (int c = 0; c < m; ++c) {
            const double a = Ar[c];
            if (!std::isfinite(a)) return false;
            s = std::max(s, std::fabs(a));
        }
        if (!(s > 0) || !std::isfinite(s)) return false;
        rs[r] = s;
    }
    CD_LU_UNROLL
    for (int k = 0; k < m; ++k) {
        int p = k;
        double best = -1;
        CD_LU_UNROLL
        for (int r = k; r < m; ++r) {
            const double q = std::fabs(A[std::size_t(r) * m + k]) / rs[r];
            if (!std::isfinite(q)) return false;
            if (q > best) { best = q; p = r; }
        }
        if (!(best > 1e-13)) return false;  // singular relative to the row's scale
        double* Ak = A + std::size_t(k) * m;
        if (p != k) {
            double* Ap = A + std::size_t(p) * m;
            CD_LU_UNROLL
            for (int c = 0; c < m; ++c) std::swap(Ak[c], Ap[c]);
            std::swap(b[k], b[p]);
            std::swap(rs[k], rs[p]);
        }
        const double piv = Ak[k];
        if (!std::isfinite(piv) || piv == 0) return false;
        const double bk = b[k];
        CD_LU_UNROLL
        for (int r = k + 1; r < m; ++r) {
            double* Ar = A + std::size_t(r) * m;
            const double f = Ar[k] / piv;
            if (f == 0) continue;
            CD_LU_UNROLL
            for (int c = k; c < m; ++c) Ar[c] -= f * Ak[c];
            b[r] -= f * bk;
        }
    }
    CD_LU_UNROLL
    for (int k = m - 1; k >= 0; --k) {
        const double* Ak = A + std::size_t(k) * m;
        double s = b[k];
        CD_LU_UNROLL
        for (int c = k + 1; c < m; ++c) s -= Ak[c] * b[c];
        b[k] = s / Ak[k];
    }
    CD_LU_UNROLL
    for (int k = 0, mm = m * m; k < mm; ++k)
        if (!std::isfinite(A[k])) return false;
    CD_LU_UNROLL
    for (int k = 0; k < m; ++k)
        if (!std::isfinite(b[k])) return false;
    return true;
}

#undef CD_LU_UNROLL

bool MnaSolver::newton(int maxIter, bool dc) noexcept {
    CD_MNA_DISPATCH(newtonT<M>(maxIter, dc))
}

template <int M>
bool MnaSolver::newtonT(int maxIter, bool dc) noexcept {
    const int m = M > 0 ? M : m_;
    const double* unit = unitScale_.data();
    // haveEval: F_/J_ already hold the residual/Jacobian at x_ (from the accepted damped trial,
    // evaluated at exactly that point in the same step context), res0 its scaled residual.
    bool haveEval = false;
    double res0 = INFINITY;
    for (int it = 1; it <= maxIter; ++it) {
        if (!haveEval) {
            evalT<M>(x_.data(), F_.data(), J_.data(), dc);
            res0 = resT<M>(F_.data());
            if (!std::isfinite(res0)) return false;
        }
        haveEval = false;
        double* x = x_.data();
        double* dx = dx_.data();
        const double* F = F_.data();
        for (int k = 0; k < m; ++k) dx[k] = -F[k];
        if (!luFixed<M>(J_.data(), dx, m)) return false;
        double stepRel = 0;
        for (int k = 0; k < m; ++k) {
            // Compute |dx|/(|x|+unit) without overflowing the denominator.
            const double scale = std::max(std::fabs(x[k]), unit[k]);
            const double rel = (std::fabs(dx[k]) / scale) / (std::fabs(x[k]) / scale + unit[k] / scale);
            if (!std::isfinite(rel)) return false;
            stepRel = std::max(stepRel, rel);
        }
        double* xt = xTry_.data();
        if (res0 <= tol_ && stepRel <= tol_) {
            // Retain the final correction, but validate the state and its actual residual.
            for (int k = 0; k < m; ++k) xt[k] = x[k] + dx[k];
            if (!finiteT<M>(xt)) return false;
            evalT<M>(xt, Ftry_.data(), nullptr, dc);
            const double res = resT<M>(Ftry_.data());
            if (!std::isfinite(res) || res > tol_) return false;
            std::swap(x_, xTry_);
            lastIter_ = it; lastRes_ = res;
            return true;
        }
        // The LU consumed J_; each trial evaluates F and J at its point so the accepted trial's
        // Jacobian is the next iteration's Newton Jacobian (identical to re-evaluating it there).
        double lambda = 1.0, res = INFINITY;
        for (int trial = 0; trial < 4; ++trial, lambda *= 0.5) {
            for (int k = 0; k < m; ++k) xt[k] = x[k] + lambda * dx[k];
            res = INFINITY;
            if (finiteT<M>(xt)) {
                evalT<M>(xt, Ftry_.data(), J_.data(), dc);
                res = resT<M>(Ftry_.data());
            }
            if (res < res0 || (trial == 3 && std::isfinite(res))) break;
        }
        if (!std::isfinite(res)) return false;
        std::swap(x_, xTry_);
        std::swap(F_, Ftry_);
        if (res <= tol_ && lambda * stepRel <= tol_) { lastIter_ = it; lastRes_ = res; return true; }
        res0 = res;
        haveEval = true;
    }
    lastIter_ = maxIter;
    return false;
}

bool MnaSolver::step() noexcept {
    if (m_ == 0) return false;
    xSave_ = x_;  // same size: element copy, no allocation
    // newton() only commits iterates that passed finiteSolution(), so a successful solve is finite.
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
    std::copy(x_.begin(), x_.end(), out);
    std::copy(qOld_.begin(), qOld_.end(), out + m_);
}

void MnaSolver::setState(const double* in) noexcept {
    if (!in || !finiteSolution(in)) return;
    for (std::size_t k = 0; k < qOld_.size(); ++k)
        if (!std::isfinite(in[m_ + k])) return;
    std::copy(in, in + m_, x_.begin());
    std::copy(in + m_, in + m_ + qOld_.size(), qOld_.begin());
}

bool MnaSolver::finiteSolution(const double* x) const noexcept {
    CD_MNA_DISPATCH(finiteT<M>(x))
}

template <int M>
bool MnaSolver::finiteT(const double* x) const noexcept {
    const int m = M > 0 ? M : m_;
    double maxAbs = 0;
    for (int k = 0; k < m; ++k) {
        if (!std::isfinite(x[k])) return false;
        maxAbs = std::max(maxAbs, std::fabs(x[k]));
    }
    // Exact shortcut: with |x| <= 2^500 and every C <= 2^500, each terminal difference is at most
    // 2^501 and each charge at most 2^1001 in magnitude, so the per-element checks below would all
    // pass. Otherwise fall through to them.
    if (maxAbs <= 0x1p500 && capMax_ <= 0x1p500) return true;
    const Stamp* st = st_.data();
    for (std::size_t k = 0, count = st_.size(); k < count; ++k) {
        const Stamp& s = st[k];
        const double v = (s.ia >= 0 ? x[s.ia] : 0.0) - (s.ib >= 0 ? x[s.ib] : 0.0);
        if (!std::isfinite(v) || (s.kind == Stamp::Capacitor && !std::isfinite(s.p0 * v))) return false;
    }
    return true;
}

void MnaSolver::storeCharges() noexcept {
    // finiteSolution checks every charge before any is committed.
    const double* x = x_.data();
    for (const Stamp& s : st_)
        if (s.kind == Stamp::Capacitor) qOld_[std::size_t(s.aux)] = s.p0 * ((s.ia >= 0 ? x[s.ia] : 0.0) - (s.ib >= 0 ? x[s.ib] : 0.0));
}

#undef CD_MNA_DISPATCH

}  // namespace cd::dsp
