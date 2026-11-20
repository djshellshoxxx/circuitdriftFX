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

// Overflow-safe softplus log(1+e^z) and logistic sigmoid (its derivative), one exp for both.
inline void softplusSig(double z, double& sp, double& sg) noexcept {
    const double e = std::exp(-std::fabs(z));
    sp = std::max(z, 0.0) + std::log1p(e);
    sg = z >= 0 ? 1.0 / (1.0 + e) : e / (1.0 + e);
}
// Tube grid conduction ig = k*(knee*softplus(vgk/knee))^1.5. Below z=-30 the current is below
// k*(knee*1e-13)^1.5 (<1e-20 A for manifest bounds) and is evaluated as exactly zero.
inline void gridCurrent(double vgk, double k, double knee, double& ig, double& dig) noexcept {
    const double z = vgk / knee;
    if (z < -30.0) { ig = 0; dig = 0; return; }
    double sp, sg;
    softplusSig(z, sp, sg);
    const double spg = knee * sp, sq = std::sqrt(spg);
    ig = k * spg * sq;
    dig = 1.5 * k * sq * sg;
}
// u^p with an exact multiplication path for small integer exponents.
inline double powFast(double u, double p) noexcept {
    const int ip = int(p);
    if (double(ip) == p && ip >= 1 && ip <= 12) {
        double r = u;
        for (int k = 1; k < ip; ++k) r *= u;
        return r;
    }
    return std::pow(u, p);
}
// exp(arg) with a C1 linear extension beyond the documented exponent cap: value and derivative.
inline void expCap(double arg, double cap, double& v, double& d) noexcept {
    if (arg > cap) {
        const double e = std::exp(cap);
        v = e * (1.0 + arg - cap); d = e;
    } else {
        v = std::exp(arg); d = v;
    }
}

// Multi-terminal device equations. v: pin voltages; I[p]: current from pin p's node into the
// device; G[p*4+q] = dI[p]/dV[q]. SYNTHETIC equation forms (textbook), constants from manifests.
void triode(const double* p, const double* v, double* I, double* G) noexcept {
    const double mu = p[0], ex = p[1], kg1 = p[2], kp = p[3], kvb = p[4], kgc = p[5], knee = p[6];
    const double vgk = v[0] - v[2], vpk = v[1] - v[2];
    double ip = 0, dg = 0, dp = 0;
    if (vpk > 0) {
        const double s = std::sqrt(kvb + vpk * vpk);
        const double z = kp * (1.0 / mu + vgk / s);
        double sp, sg;
        softplusSig(z, sp, sg);
        const double e1 = vpk / kp * sp;
        if (e1 > 0) {
            const double pw = std::pow(e1, ex - 1.0);
            ip = 2.0 * pw * e1 / kg1;
            const double dide = 2.0 * ex * pw / kg1;
            dg = dide * (vpk * sg / s);
            dp = dide * (sp / kp - vpk * sg * vgk * vpk / (s * s * s));
        }
    }
    double ig, dig;
    gridCurrent(vgk, kgc, knee, ig, dig);
    I[0] = ig; I[1] = ip; I[2] = -(ig + ip);
    G[0] = dig; G[1] = 0; G[2] = -dig;
    G[4] = dg; G[5] = dp; G[6] = -(dg + dp);
    for (int q = 0; q < 3; ++q) G[8 + q] = -(G[q] + G[4 + q]);
}

void pentode(const double* p, const double* v, double* I, double* G) noexcept {
    const double mu = p[0], ex = p[1], kg1 = p[2], kg2 = p[3], kp = p[4], kvb = p[5], kgc = p[6], knee = p[7];
    const double vgk = v[0] - v[2], vpk = v[1] - v[2], vsk = v[3] - v[2];
    double ip = 0, dpg = 0, dpp = 0, dps = 0, is = 0, dsg = 0, dss = 0;
    if (vsk > 0) {
        const double z = kp * (1.0 / mu + vgk / vsk);
        double sp, sg;
        softplusSig(z, sp, sg);
        const double e1 = vsk / kp * sp;
        if (e1 > 0) {
            const double deg = sg, des = sp / kp - sg * vgk / vsk;
            const double pw = std::pow(e1, ex - 1.0);
            const double f = 2.0 * pw * e1, df = 2.0 * ex * pw;
            constexpr double v0 = 1.0;  // softplus knee width of the plate knee argument [V]
            double q, dq;
            softplusSig(vpk / v0, q, dq);
            q *= v0;
            const double kn = std::atan(q / kvb), dkn = (1.0 / kvb) / (1.0 + (q / kvb) * (q / kvb)) * dq;
            ip = f / kg1 * kn;
            dpg = df * deg / kg1 * kn; dps = df * des / kg1 * kn; dpp = f / kg1 * dkn;
            is = f / kg2;
            dsg = df * deg / kg2; dss = df * des / kg2;
        }
    }
    double ig, dig;
    gridCurrent(vgk, kgc, knee, ig, dig);
    I[0] = ig; I[1] = ip; I[3] = is; I[2] = -(ig + ip + is);
    G[0] = dig; G[1] = 0; G[2] = -dig; G[3] = 0;
    G[4] = dpg; G[5] = dpp; G[6] = -(dpg + dpp + dps); G[7] = dps;
    G[12] = dsg; G[13] = 0; G[14] = -(dsg + dss); G[15] = dss;
    for (int q = 0; q < 4; ++q) G[8 + q] = -(G[q] + G[4 + q] + G[12 + q]);
}

void npn(const double* p, double cap, const double* v, double* I, double* G) noexcept {
    const double is = p[0], bf = p[1], br = p[2], vt = p[3];
    const double vbe = v[0] - v[2], vbc = v[0] - v[1];
    double ef, def, er, der;
    expCap(vbe / vt, cap, ef, def);
    expCap(vbc / vt, cap, er, der);
    const double ic = is * (ef - er) - is / br * (er - 1.0);
    const double ib = is / bf * (ef - 1.0) + is / br * (er - 1.0);
    const double gf = is * def / vt, gr = is * der / vt;
    const double dicBe = gf, dicBc = -gr - gr / br, dibBe = gf / bf, dibBc = gr / br;
    I[0] = ib; I[1] = ic; I[2] = -(ib + ic);
    G[0] = dibBe + dibBc; G[1] = -dibBc; G[2] = -dibBe;
    G[4] = dicBe + dicBc; G[5] = -dicBc; G[6] = -dicBe;
    for (int q = 0; q < 3; ++q) G[8 + q] = -(G[q] + G[4 + q]);
}

void njfet(const double* p, double cap, const double* v, double* I, double* G) noexcept {
    const double beta = p[0], vp = p[1], lam = p[2], knee = p[3], isg = p[4], vt = p[5];
    const double vgs = v[0] - v[2], vds = v[1] - v[2];
    double ve, dve;
    softplusSig((vgs + vp) / knee, ve, dve);
    ve *= knee;
    double id, dgs, dds;
    if (vds >= 0) {
        const double clm = 1.0 + lam * vds;
        double core, dcve, dcds;
        if (vds < ve) { core = 2.0 * ve * vds - vds * vds; dcve = 2.0 * vds; dcds = 2.0 * ve - 2.0 * vds; }
        else { core = ve * ve; dcve = 2.0 * ve; dcds = 0.0; }
        id = beta * core * clm;
        dgs = beta * dcve * dve * clm;
        dds = beta * (dcds * clm + core * lam);
    } else {
        id = 2.0 * beta * ve * vds; dgs = 2.0 * beta * dve * vds; dds = 2.0 * beta * ve;
    }
    double eg, deg;
    expCap(vgs / vt, cap, eg, deg);
    const double ig = isg * (eg - 1.0), dig = isg * deg / vt;
    I[0] = ig; I[1] = id; I[2] = -(ig + id);
    G[0] = dig; G[1] = 0; G[2] = -dig;
    G[4] = dgs; G[5] = dds; G[6] = -(dgs + dds);
    for (int q = 0; q < 3; ++q) G[8 + q] = -(G[q] + G[4 + q]);
}

// Saturating magnetizing current i(lambda) and di/dlambda with a C1 linear extension at the cap.
inline void inductorCurrent(const double* p, double capU, double lam, double& i, double& g) noexcept {
    const double L = p[0], ls = p[1], pe = p[2];
    const double u = std::fabs(lam / ls);
    if (u <= capU) {
        const double up = powFast(u, pe);
        i = lam / L * (1.0 + up);
        g = (1.0 + (pe + 1.0) * up) / L;
    } else {
        const double up = powFast(capU, pe), lc = std::copysign(capU * ls, lam);
        g = (1.0 + (pe + 1.0) * up) / L;
        i = lc / L * (1.0 + up) + g * (lam - lc);
    }
}
}  // namespace

bool MnaSolver::prepare(const Netlist& net, double h, const Options& opt, std::string& err) {
    MnaSolver prepared;
    if (!prepared.prepareInternal(net, h, opt, err)) return false;
    *this = std::move(prepared);
    return true;
}

int MnaSolver::pins(std::size_t k, int* node) const noexcept {
    const NetElement& e = el_[k];
    node[0] = e.a; node[1] = e.b; node[2] = e.c; node[3] = e.d;
    switch (cat_[k]) {
        case Cat::Triode: case Cat::Npn: case Cat::Njfet: return 3;
        case Cat::Pentode: return 4;
        default: return 2;
    }
}

bool MnaSolver::prepareInternal(const Netlist& net, double h, const Options& opt, std::string& err) {
    auto bad = [&](const char* why) { err = why; m_ = 0; return false; };
    if (!(h > 0 && std::isfinite(h))) return bad("substep h must be positive and finite");
    if (opt.maxNewton < 1 || opt.maxNewton > 12 || !(opt.tol > 0 && opt.tol <= 1e-7 && std::isfinite(opt.tol)))
        return bad("Newton options out of range (finite 0 < tolerance <= 1e-7 required)");
    if (net.nodes < 1 || net.nodes > kMaxNodes) return bad("node count outside 1..32");
    if (net.elements.empty() || net.elements.size() > kMaxElements) return bad("element count outside 1..68");
    if (!(net.voltageScaleV > 0 && net.currentScaleA > 0 && std::isfinite(net.voltageScaleV) && std::isfinite(net.currentScaleA)))
        return bad("circuit scales must be strictly positive");

    int vs = 0, nonlinear = 0, components = 0, portSources = 0;
    std::vector<int> dcSet(net.nodes + 1), vsSet(net.nodes + 1);
    std::iota(dcSet.begin(), dcSet.end(), 0);
    std::iota(vsSet.begin(), vsSet.end(), 0);
    std::vector<Cat> cats;
    auto join = [&](int p, int q) { dcSet[findRoot(dcSet, p)] = findRoot(dcSet, q); };
    for (const auto& e : net.elements) {
        if (e.internalPortSource) {
            if (e.kind == NetElement::Kind::Device || ++portSources > kMaxPortSources)
                return bad("invalid internal port source budget");
        } else if (++components > kMaxComponents) return bad("more than 64 user components");
        if (e.a < 0 || e.b < 0 || e.a > net.nodes || e.b > net.nodes || e.a == e.b) return bad("element node index invalid");
        if (e.kind == NetElement::Kind::Device) {
            if (!e.model || e.model->params.size() > std::size_t(NetElement::kMaxParams)) return bad("device without supported manifest");
            std::size_t paramCount = 0;
            int pinCount = 2;
            Cat c = Cat::Res;
            switch (e.model->equation) {
                case DeviceEquation::Resistor: c = Cat::Res; paramCount = 1; break;
                case DeviceEquation::Capacitor: c = Cat::Cap; paramCount = 1; break;
                case DeviceEquation::ShockleyDiode: c = Cat::Diode; paramCount = 3; break;
                case DeviceEquation::Electrolytic: c = Cat::Elec; paramCount = 3; break;
                case DeviceEquation::SaturatingInductor: c = Cat::Ind; paramCount = 4; break;
                case DeviceEquation::KorenTriode: c = Cat::Triode; paramCount = 7; pinCount = 3; break;
                case DeviceEquation::KorenPentode: c = Cat::Pentode; paramCount = 8; pinCount = 4; break;
                case DeviceEquation::EbersMollNpn: c = Cat::Npn; paramCount = 4; pinCount = 3; break;
                case DeviceEquation::SquareLawNjfet: c = Cat::Njfet; paramCount = 6; pinCount = 3; break;
                default: return bad("unsupported device equation");
            }
            if (int(e.model->pins.size()) != pinCount) return bad("device without supported manifest");
            if (e.model->params.size() != paramCount) return bad("device parameter count does not match equation");
            const int pn[4] = {e.a, e.b, e.c, e.d};
            for (int q = 2; q < pinCount; ++q)
                if (pn[q] < 0 || pn[q] > net.nodes) return bad("element node index invalid");
            for (int q = 0; q < pinCount; ++q)
                for (int r = q + 1; r < pinCount; ++r)
                    if (pn[q] == pn[r]) return bad("element node index invalid");
            for (std::size_t k = 0; k < e.model->params.size(); ++k) {
                const auto& p = e.model->params[k];
                if (!(std::isfinite(p.min) && std::isfinite(p.max) && p.min > 0 &&
                      std::isfinite(e.param[k]) && e.param[k] >= p.min && e.param[k] <= p.max))
                    return bad("device parameter outside finite positive manifest bounds");
            }
            if (c == Cat::Res && !std::isfinite(1.0 / e.param[0])) return bad("resistor conductance exceeds numeric range");
            if ((c == Cat::Cap || c == Cat::Elec) && !std::isfinite(e.param[0] / h))
                return bad("capacitor step conductance exceeds numeric range");
            if (c == Cat::Elec && !(std::isfinite(1.0 / e.param[1]) && std::isfinite(1.0 / e.param[2])))
                return bad("electrolytic conductance exceeds numeric range");
            if (c == Cat::Ind && !(std::isfinite(h / e.param[0]) && std::isfinite(1.0 / e.param[3]) &&
                                   e.model->maxNormalizedFlux > 0 && e.model->maxNormalizedFlux <= 80))
                return bad("inductor domain exceeds numeric range");
            if (c == Cat::Diode &&
                (!(e.model->maxExpArg > 0 && e.model->maxExpArg <= 80 && std::isfinite(e.model->maxExpArg)) ||
                 !(e.param[1] * e.param[2] > 0 && std::isfinite(e.param[1] * e.param[2]))))
                return bad("diode domain exceeds numeric range");
            if ((c == Cat::Npn || c == Cat::Njfet) &&
                !(e.model->maxExpArg > 0 && e.model->maxExpArg <= 80 && std::isfinite(e.model->maxExpArg)))
                return bad("junction domain exceeds numeric range");
            // DC conduction paths used by the floating-node check (necessary, not sufficient).
            switch (c) {
                case Cat::Cap: break;
                case Cat::Triode: join(e.b, e.c); break;
                case Cat::Pentode: join(e.b, e.c); join(e.d, e.c); break;
                case Cat::Npn: join(e.a, e.c); join(e.b, e.c); break;
                case Cat::Njfet: join(e.b, e.c); break;
                default: join(e.a, e.b); break;
            }
            if (c == Cat::Diode || c == Cat::Ind || c == Cat::Triode || c == Cat::Pentode || c == Cat::Npn || c == Cat::Njfet)
                if (++nonlinear > kMaxNonlinear) return bad("more than 8 nonlinear devices");
            cats.push_back(c);
        } else {
            if (e.kind != NetElement::Kind::VoltageSource && e.kind != NetElement::Kind::CurrentSource)
                return bad("unsupported source kind");
            if (!std::isfinite(e.value)) return bad("non-finite source value");
            if (e.kind == NetElement::Kind::VoltageSource) {
                ++vs;
                join(e.a, e.b);
                const int ra = findRoot(vsSet, e.a), rb = findRoot(vsSet, e.b);
                if (ra == rb) return bad("ideal voltage-source loop");
                vsSet[ra] = rb;
                cats.push_back(Cat::VSrc);
            } else {
                cats.push_back(Cat::ISrc);
            }
        }
    }
    for (int i = 1; i <= net.nodes; ++i)
        if (findRoot(dcSet, i) != findRoot(dcSet, 0)) return bad("node without DC path to ground (floating)");
    if (net.nodes + vs > kMaxUnknowns) return bad("more than 96 unknowns");

    el_ = net.elements;
    cat_ = cats;
    n_ = net.nodes;
    m_ = n_ + vs;
    h_ = h; tol_ = opt.tol; maxNewton_ = opt.maxNewton;
    vScale_ = net.voltageScaleV; iScale_ = net.currentScaleA;
    vsRow_.assign(el_.size(), -1);
    capSlot_.assign(el_.size(), -1);
    nonlinear_.clear();
    stateful_.clear();
    rhsElems_.clear();
    int row = n_, states = 0;
    for (std::size_t k = 0; k < el_.size(); ++k) {
        const Cat c = cat_[k];
        if (c == Cat::VSrc) vsRow_[k] = row++;
        else if (c == Cat::Cap || c == Cat::Elec || c == Cat::Ind) { capSlot_[k] = states++; stateful_.push_back(int(k)); }
        if (c == Cat::Diode || c == Cat::Ind || c == Cat::Triode || c == Cat::Pentode || c == Cat::Npn || c == Cat::Njfet)
            nonlinear_.push_back(int(k));
        if (c == Cat::Cap || c == Cat::Elec || c == Cat::ISrc || c == Cat::VSrc) rhsElems_.push_back(int(k));
    }
    qOld_.assign(states, 0.0);
    for (auto* v : {&x_, &xSave_, &xTry_, &F_, &Ftry_, &dx_, &rowScale_, &unitScale_, &rhs_}) v->assign(m_, 0.0);
    J_.assign(std::size_t(m_) * m_, 0.0);
    Jtry_.assign(std::size_t(m_) * m_, 0.0);
    Jlin_.assign(std::size_t(m_) * m_, 0.0);
    elG_.assign(el_.size(), 0.0);
    elD_.assign(el_.size(), 0.0);
    elCur_.assign(el_.size(), 0.0);
    devI_.assign(el_.size() * NetElement::kMaxPins, 0.0);
    for (int k = 0; k < m_; ++k) unitScale_[k] = k < n_ ? vScale_ : iScale_;
    failures_ = 0; consecutive_ = 0;
    linDirty_ = true;
    srcScale_ = 1.0;
    bool ok = newton(kDcIterations, true);
    if (!ok) {
        // Bounded source-stepping homotopy (prepare-time only): ramp every source 10%..100%.
        std::fill(x_.begin(), x_.end(), 0.0);
        ok = true;
        for (int s = 1; s <= 10 && ok; ++s) {
            srcScale_ = 0.1 * s;
            ok = newton(kDcIterations, true);
        }
        srcScale_ = 1.0;
    }
    if (!ok) return bad("DC operating point did not converge or matrix singular");
    if (!finiteSolution(x_.data())) return bad("DC state or capacitor charge exceeds numeric range");
    if (!initDcStates()) return bad("DC state or capacitor charge exceeds numeric range");
    err.clear();
    return true;
}

void MnaSolver::setSource(int k, double value) noexcept {
    if (k >= 0 && k < int(el_.size()) && el_[k].kind != NetElement::Kind::Device) el_[k].value = value;
}

bool MnaSolver::setElementParam(int k, int i, double value) noexcept {
    if (k < 0 || k >= int(el_.size()) || el_[k].kind != NetElement::Kind::Device) return false;
    const auto& mp = el_[k].model->params;
    if (i < 0 || i >= int(mp.size()) || !std::isfinite(value)) return false;
    value = std::clamp(value, mp[i].min, mp[i].max);
    NetElement& e = el_[k];
    if (value == e.param[i]) return true;
    const Cat c = cat_[k];
    if (c == Cat::Cap && i == 0) {
        const double q = value * (voltage(e.a) - voltage(e.b));
        if (!std::isfinite(q) || !std::isfinite(value / h_)) return false;
        qOld_[capSlot_[k]] = q;
    }
    if (c == Cat::Elec && i == 0 && !std::isfinite(value / h_)) return false;
    if (c == Cat::Ind && i == 0 && !std::isfinite(h_ / value)) return false;
    e.param[i] = value;
    if (c == Cat::Res || c == Cat::Cap || c == Cat::Elec) linDirty_ = true;
    return true;
}

double MnaSolver::elementParam(int k, int i) const noexcept {
    if (k < 0 || k >= int(el_.size()) || i < 0 || i >= NetElement::kMaxParams) return 0.0;
    return el_[k].param[i];
}

bool MnaSolver::setStepSize(double h) noexcept {
    if (m_ == 0 || !(h > 0) || !std::isfinite(h)) return false;
    for (std::size_t k = 0; k < el_.size(); ++k) {
        const Cat c = cat_[k];
        if ((c == Cat::Cap || c == Cat::Elec) && !std::isfinite(el_[k].param[0] / h)) return false;
        if (c == Cat::Ind && !std::isfinite(h / el_[k].param[0])) return false;
    }
    h_ = h;
    linDirty_ = true;
    return true;
}

void MnaSolver::setMaxNewton(int n) noexcept { maxNewton_ = std::clamp(n, 1, 12); }

// ---- residual assembly --------------------------------------------------------------------

void MnaSolver::stampNonlinear(const double* x, double* F, double* J, bool dc) noexcept {
    const int m = m_;
    auto V = [&](int node) { return node == 0 ? 0.0 : x[node - 1]; };
    for (int k : nonlinear_) {
        const NetElement& e = el_[k];
        int node[4];
        const int np = pins(std::size_t(k), node);
        double I[4] = {0, 0, 0, 0}, G[16] = {};
        const double v[4] = {V(node[0]), V(node[1]), np > 2 ? V(node[2]) : 0.0, np > 3 ? V(node[3]) : 0.0};
        switch (cat_[k]) {
            case Cat::Diode: {
                const double is = e.param[0], nvt = e.param[1] * e.param[2], cap = e.model->maxExpArg;
                const double arg = (v[0] - v[1]) / nvt;
                double i, g;
                if (arg > cap) {  // C1 linear extension beyond the documented exponent domain
                    const double ec = std::exp(cap);
                    i = is * (ec * (1.0 + arg - cap) - 1.0); g = is * ec / nvt;
                } else {
                    const double ex = std::exp(arg);
                    i = is * (ex - 1.0); g = is * ex / nvt;
                }
                I[0] = i; I[1] = -i; G[0] = g; G[1] = -g; G[4] = -g; G[5] = g;
                break;
            }
            case Cat::Ind: {
                double i, g;
                const double vab = v[0] - v[1];
                if (dc) { g = 1.0 / e.param[3]; i = g * vab; }
                else {
                    inductorCurrent(e.param, e.model->maxNormalizedFlux, qOld_[capSlot_[k]] + h_ * vab, i, g);
                    g *= h_;
                }
                I[0] = i; I[1] = -i; G[0] = g; G[1] = -g; G[4] = -g; G[5] = g;
                break;
            }
            case Cat::Triode: triode(e.param, v, I, G); break;
            case Cat::Pentode: pentode(e.param, v, I, G); break;
            case Cat::Npn: npn(e.param, e.model->maxExpArg, v, I, G); break;
            case Cat::Njfet: njfet(e.param, e.model->maxExpArg, v, I, G); break;
            default: break;
        }
        double* cache = &devI_[std::size_t(k) * NetElement::kMaxPins];
        for (int p = 0; p < np; ++p) {
            cache[p] = I[p];
            if (node[p]) F[node[p] - 1] += I[p];
            if (!J || !node[p]) continue;
            double* row = J + std::size_t(node[p] - 1) * m;
            for (int q = 0; q < np; ++q)
                if (node[q]) row[node[q] - 1] += G[p * 4 + q];
        }
    }
}

void MnaSolver::evalDc(const double* x, double* F, double* J) noexcept {
    std::fill(F, F + m_, 0.0);
    if (J) std::fill(J, J + std::size_t(m_) * m_, 0.0);
    auto V = [&](int node) { return node == 0 ? 0.0 : x[node - 1]; };
    auto addF = [&](int node, double i) { if (node) F[node - 1] += i; };
    auto addJ = [&](int r, int c, double g) { if (J && r >= 0 && c >= 0) J[std::size_t(r) * m_ + c] += g; };
    auto stamp = [&](int a, int b, double g) {
        addJ(a - 1, a - 1, g); addJ(a - 1, b - 1, -g); addJ(b - 1, a - 1, -g); addJ(b - 1, b - 1, g);
    };
    for (std::size_t k = 0; k < el_.size(); ++k) {
        const NetElement& e = el_[k];
        const double v = V(e.a) - V(e.b);
        switch (cat_[k]) {
            case Cat::Res: { const double g = 1.0 / e.param[0]; addF(e.a, g * v); addF(e.b, -g * v); stamp(e.a, e.b, g); break; }
            case Cat::Elec: { const double g = 1.0 / (e.param[1] + e.param[2]); addF(e.a, g * v); addF(e.b, -g * v); stamp(e.a, e.b, g); break; }
            case Cat::ISrc: addF(e.a, srcScale_ * e.value); addF(e.b, -srcScale_ * e.value); break;
            case Cat::VSrc: {
                const int r = vsRow_[k];
                addF(e.a, x[r]); addF(e.b, -x[r]);
                addJ(e.a - 1, r, 1.0); addJ(e.b - 1, r, -1.0);
                F[r] = v - srcScale_ * e.value;
                addJ(r, e.a - 1, 1.0); addJ(r, e.b - 1, -1.0);
                break;
            }
            default: break;  // capacitors open; nonlinear stamped below
        }
    }
    stampNonlinear(x, F, J, true);
}

void MnaSolver::refreshLinear() noexcept {
    if (!linDirty_) return;
    const int m = m_;
    std::fill(Jlin_.begin(), Jlin_.end(), 0.0);
    auto addJ = [&](int r, int c, double g) { if (r >= 0 && c >= 0) Jlin_[std::size_t(r) * m + c] += g; };
    auto stamp = [&](int a, int b, double g) {
        addJ(a - 1, a - 1, g); addJ(a - 1, b - 1, -g); addJ(b - 1, a - 1, -g); addJ(b - 1, b - 1, g);
    };
    for (std::size_t k = 0; k < el_.size(); ++k) {
        const NetElement& e = el_[k];
        switch (cat_[k]) {
            case Cat::Res: elG_[k] = 1.0 / e.param[0]; stamp(e.a, e.b, elG_[k]); break;
            case Cat::Cap: elG_[k] = e.param[0] / h_; stamp(e.a, e.b, elG_[k]); break;
            case Cat::Elec: {
                const double ch = e.param[0] / h_, ge = 1.0 / e.param[1], gl = 1.0 / e.param[2];
                elD_[k] = ch + gl + ge;
                elG_[k] = ge * (ch + gl) / elD_[k];
                stamp(e.a, e.b, elG_[k]);
                break;
            }
            case Cat::VSrc: {
                const int r = vsRow_[k];
                addJ(e.a - 1, r, 1.0); addJ(e.b - 1, r, -1.0);
                addJ(r, e.a - 1, 1.0); addJ(r, e.b - 1, -1.0);
                break;
            }
            default: break;
        }
    }
    linDirty_ = false;
}

void MnaSolver::buildRhs() noexcept {
    std::fill(rhs_.begin(), rhs_.end(), 0.0);
    auto add = [&](int node, double i) { if (node) rhs_[node - 1] += i; };
    for (int k : rhsElems_) {
        const NetElement& e = el_[std::size_t(k)];
        switch (cat_[std::size_t(k)]) {
            case Cat::Cap: { const double i = qOld_[capSlot_[k]] / h_; add(e.a, -i); add(e.b, i); break; }
            case Cat::Elec: {
                const double ih = (1.0 / e.param[1]) * (e.param[0] / h_) * qOld_[capSlot_[k]] / elD_[k];
                add(e.a, -ih); add(e.b, ih);
                break;
            }
            case Cat::ISrc: add(e.a, e.value); add(e.b, -e.value); break;
            case Cat::VSrc: rhs_[vsRow_[k]] -= e.value; break;
            default: break;
        }
    }
}

void MnaSolver::evalFast(const double* x, double* F, double* J) noexcept {
    const int m = m_;
    for (int r = 0; r < m; ++r) {
        const double* row = &Jlin_[std::size_t(r) * m];
        double s = rhs_[r];
        for (int c = 0; c < m; ++c) s += row[c] * x[c];
        F[r] = s;
    }
    if (J) std::copy(Jlin_.begin(), Jlin_.end(), J);
    stampNonlinear(x, F, J, false);
}

void MnaSolver::evaluate(const double* x, double* F, double* J, bool dc) noexcept {
    if (m_ == 0) return;
    if (dc) { evalDc(x, F, J); return; }
    refreshLinear();
    buildRhs();
    evalFast(x, F, J);
}

double MnaSolver::scaledResidual(const double* F) const noexcept {
    double r = 0;
    for (int k = 0; k < m_; ++k) {
        const double s = std::fabs(F[k]) / (k < n_ ? iScale_ : vScale_);
        if (!std::isfinite(s)) return INFINITY;
        r = std::max(r, s);
    }
    return r;
}

#if defined(__clang__)
#define CD_UNROLL _Pragma("unroll")
#elif defined(__GNUC__)
#define CD_UNROLL _Pragma("GCC unroll 8")
#else
#define CD_UNROLL
#endif

namespace {
// Scaled partial-pivot LU solve of A x = b (b overwritten). Finiteness is checked on entry (every
// coefficient), on every pivot and on the solution; intermediate overflow propagates to a
// non-finite solution (rejected). Every accepted Newton result is additionally re-validated by its
// actual residual. N > 0: compile-time size (unrolled kernels); N == 0: runtime size m.
template <int N>
bool luKernel(double* A, double* b, double* rowInv, int mRuntime) noexcept {
    const int m = N > 0 ? N : mRuntime;
    double probe = 0;
    for (int r = 0; r < m; ++r) {
        probe += b[r] * 0.0;
        double s = 0;
        const double* row = A + std::size_t(r) * m;
        for (int c = 0; c < m; ++c) { probe += row[c] * 0.0; s = std::max(s, std::fabs(row[c])); }
        rowInv[r] = 1.0 / s;
    }
    if (!(probe == 0.0)) return false;
    for (int r = 0; r < m; ++r) if (!(rowInv[r] > 0) || !std::isfinite(rowInv[r])) return false;
    CD_UNROLL for (int k = 0; k < m; ++k) {
        int p = k;
        double best = -1;
        CD_UNROLL for (int r = k; r < m; ++r) {
            const double q = std::fabs(A[std::size_t(r) * m + k]) * rowInv[r];
            if (q > best) { best = q; p = r; }
        }
        if (!(best > 1e-13) || !std::isfinite(best)) return false;  // singular relative to the row's scale
        if (p != k) {
            for (int c = 0; c < m; ++c) std::swap(A[std::size_t(k) * m + c], A[std::size_t(p) * m + c]);
            std::swap(b[k], b[p]);
            std::swap(rowInv[k], rowInv[p]);
        }
        const double* pr = A + std::size_t(k) * m;
        const double inv = 1.0 / pr[k];
        if (!std::isfinite(inv)) return false;
        CD_UNROLL for (int r = k + 1; r < m; ++r) {
            double* rr = A + std::size_t(r) * m;
            const double f = rr[k] * inv;
            if (f == 0) continue;  // structurally sparse rows; NaN never compares equal
            CD_UNROLL for (int c = k; c < m; ++c) rr[c] -= f * pr[c];
            b[r] -= f * b[k];
        }
    }
    CD_UNROLL for (int k = m - 1; k >= 0; --k) {
        const double* row = A + std::size_t(k) * m;
        double s = b[k];
        CD_UNROLL for (int c = k + 1; c < m; ++c) s -= row[c] * b[c];
        b[k] = s / row[k];
    }
    probe = 0;
    for (int k = 0; k < m; ++k) probe += b[k] * 0.0;
    return probe == 0.0;
}
}  // namespace

bool MnaSolver::luSolve(double* A, double* b) noexcept {
    double* ri = rowScale_.data();
    switch (m_) {
        case 1: return luKernel<1>(A, b, ri, 1);
        case 2: return luKernel<2>(A, b, ri, 2);
        case 3: return luKernel<3>(A, b, ri, 3);
        case 4: return luKernel<4>(A, b, ri, 4);
        case 5: return luKernel<5>(A, b, ri, 5);
        case 6: return luKernel<6>(A, b, ri, 6);
        case 7: return luKernel<7>(A, b, ri, 7);
        case 8: return luKernel<8>(A, b, ri, 8);
        default: return luKernel<0>(A, b, ri, m_);
    }
}

bool MnaSolver::newton(int maxIter, bool dc) noexcept {
    if (!dc) { refreshLinear(); buildRhs(); }

    auto evalAt = [&](const double* x, double* F, double* J) {  // residual assembly (not script evaluation)
        if (dc) evalDc(x, F, J); else evalFast(x, F, J);
    };
    // When the undamped trial is accepted, its residual/Jacobian (computed at the same point the
    // next iteration starts from) are reused: identical values, one device evaluation saved.
    bool haveFJ = false;
    for (int it = 1; it <= maxIter; ++it) {
        if (!haveFJ) evalAt(x_.data(), F_.data(), J_.data());
        haveFJ = false;
        const double res0 = scaledResidual(F_.data());
        if (!std::isfinite(res0)) { lastFailure_ = FailReason::NonFiniteResidual; return false; }
        for (int k = 0; k < m_; ++k) dx_[k] = -F_[k];
        if (!luSolve(J_.data(), dx_.data())) { lastFailure_ = FailReason::SingularJacobian; return false; }
        double stepRel = 0;
        for (int k = 0; k < m_; ++k) {
            // |dx|/(|x|+unit); the rescaled form only when the denominator would overflow.
            const double den = std::fabs(x_[k]) + unitScale_[k];
            double rel = std::fabs(dx_[k]) / den;
            if (!std::isfinite(den)) {
                const double scale = std::max(std::fabs(x_[k]), unitScale_[k]);
                rel = (std::fabs(dx_[k]) / scale) / (std::fabs(x_[k]) / scale + unitScale_[k] / scale);
            }
            if (!std::isfinite(rel)) { lastFailure_ = FailReason::NonFiniteStep; return false; }
            stepRel = std::max(stepRel, rel);
        }
        if (res0 <= tol_ && stepRel <= tol_) {
            // Retain the final correction, but validate the state and its actual residual.
            for (int k = 0; k < m_; ++k) xTry_[k] = x_[k] + dx_[k];
            if (!finiteSolution(xTry_.data())) { lastFailure_ = FailReason::NonFiniteStep; return false; }
            evalAt(xTry_.data(), Ftry_.data(), nullptr);
            const double res = scaledResidual(Ftry_.data());
            if (!std::isfinite(res) || res > tol_) { lastFailure_ = FailReason::FinalResidual; return false; }
            std::swap(x_, xTry_);
            lastIter_ = it; lastRes_ = res;
            return true;
        }
        double lambda = 1.0, res = INFINITY;
        int accepted = -1;
        for (int trial = 0; trial < 4; ++trial, lambda *= 0.5) {
            for (int k = 0; k < m_; ++k) xTry_[k] = x_[k] + lambda * dx_[k];
            res = INFINITY;
            if (finiteSolution(xTry_.data())) {
                evalAt(xTry_.data(), Ftry_.data(), (trial == 0 && it < maxIter) ? Jtry_.data() : nullptr);
                res = scaledResidual(Ftry_.data());
            }
            if (res < res0 || (trial == 3 && std::isfinite(res))) { accepted = trial; break; }
        }
        if (!std::isfinite(res)) { lastFailure_ = FailReason::NonFiniteTrial; return false; }
        std::swap(x_, xTry_);
        if (res <= tol_ && lambda * stepRel <= tol_) { lastIter_ = it; lastRes_ = res; return true; }
        if (accepted == 0 && it < maxIter) { std::swap(F_, Ftry_); std::swap(J_, Jtry_); haveFJ = true; }
    }
    lastIter_ = maxIter;
    lastFailure_ = FailReason::IterationCap;
    return false;
}

bool MnaSolver::step() noexcept {
    if (m_ == 0) return false;
    lastFailure_ = FailReason::None;
    xSave_ = x_;  // same size: element copy, no allocation
    if (!newton(maxNewton_, false) || !finiteSolution(x_.data())) {
        x_ = xSave_;
        if (failures_ < std::numeric_limits<long>::max()) ++failures_;
        if (consecutive_ < 3) ++consecutive_;
        return false;
    }
    commitStep();
    consecutive_ = 0;
    return true;
}

void MnaSolver::commitStep() noexcept {
    // finiteSolution checks every resulting state before any is committed. Only stateful
    // elements need committed currents here; resistor/source/diode readouts are derived lazily.
    for (int k : stateful_) {
        const NetElement& e = el_[std::size_t(k)];
        const double v = voltage(e.a) - voltage(e.b);
        double& st = qOld_[capSlot_[std::size_t(k)]];
        switch (cat_[std::size_t(k)]) {
            case Cat::Cap: {
                const double q = e.param[0] * v;
                elCur_[k] = (q - st) / h_;
                st = q;
                break;
            }
            case Cat::Elec: {
                const double ge = 1.0 / e.param[1], ch = e.param[0] / h_;
                const double vcNew = (ch * st + ge * v) / elD_[k];
                elCur_[k] = ge * (v - vcNew);
                st = vcNew;
                break;
            }
            default:  // inductor: committed current from the device cache at the accepted point
                st += h_ * v;
                elCur_[k] = devI_[std::size_t(k) * NetElement::kMaxPins];
                break;
        }
    }
}

bool MnaSolver::solveOperatingPoint() noexcept {
    if (m_ == 0) return false;
    xSave_ = x_;
    bool ok = newton(kDcIterations, true);
    if (!ok) {
        std::fill(x_.begin(), x_.end(), 0.0);
        ok = true;
        for (int s = 1; s <= 10 && ok; ++s) { srcScale_ = 0.1 * s; ok = newton(kDcIterations, true); }
        srcScale_ = 1.0;
    }
    if (!ok || !finiteSolution(x_.data()) || !initDcStates()) { x_ = xSave_; return false; }
    return true;
}

bool MnaSolver::initDcStates() noexcept {
    for (std::size_t k = 0; k < el_.size(); ++k) {
        const NetElement& e = el_[k];
        const double v = voltage(e.a) - voltage(e.b);
        switch (cat_[k]) {
            case Cat::Res: elCur_[k] = v / e.param[0]; break;
            case Cat::Cap: qOld_[capSlot_[k]] = e.param[0] * v; elCur_[k] = 0; break;
            case Cat::Elec: {
                const double i = v / (e.param[1] + e.param[2]);
                qOld_[capSlot_[k]] = v - i * e.param[1];
                elCur_[k] = i;
                break;
            }
            case Cat::Ind: {
                const double i = v / e.param[3];
                qOld_[capSlot_[k]] = inductorFlux(k, i);
                elCur_[k] = i;
                break;
            }
            case Cat::Diode: elCur_[k] = devI_[k * NetElement::kMaxPins]; break;
            case Cat::ISrc: elCur_[k] = e.value; break;
            case Cat::VSrc: elCur_[k] = x_[vsRow_[k]]; break;
            default: break;
        }
        if (capSlot_[k] >= 0 && !std::isfinite(qOld_[capSlot_[k]])) return false;
    }
    linDirty_ = true;
    return true;
}

double MnaSolver::inductorFlux(std::size_t k, double i) const noexcept {
    // Invert the odd, strictly increasing magnetizing curve by bracketing + bisection (prepare only).
    const NetElement& e = el_[k];
    if (i == 0 || !std::isfinite(i)) return 0.0;
    double lo = 0, hi = e.param[1], ii, g;
    for (int s = 0; s < 200; ++s) {
        inductorCurrent(e.param, e.model->maxNormalizedFlux, std::copysign(hi, i), ii, g);
        if (std::fabs(ii) >= std::fabs(i)) break;
        lo = hi; hi *= 2;
    }
    for (int s = 0; s < 200; ++s) {
        const double mid = 0.5 * (lo + hi);
        inductorCurrent(e.param, e.model->maxNormalizedFlux, mid, ii, g);
        (ii > std::fabs(i) ? hi : lo) = mid;
    }
    return std::copysign(0.5 * (lo + hi), i);
}

double MnaSolver::pinCurrent(int k, int pin) const noexcept {
    if (k < 0 || k >= int(el_.size()) || pin < 0 || pin >= NetElement::kMaxPins) return 0.0;
    const NetElement& e = el_[std::size_t(k)];
    double i;
    switch (cat_[k]) {
        case Cat::Triode: case Cat::Pentode: case Cat::Npn: case Cat::Njfet:
            return devI_[std::size_t(k) * NetElement::kMaxPins + pin];
        case Cat::Res: i = (voltage(e.a) - voltage(e.b)) / e.param[0]; break;
        case Cat::Diode: i = devI_[std::size_t(k) * NetElement::kMaxPins]; break;
        case Cat::ISrc: i = e.value; break;
        case Cat::VSrc: i = x_[std::size_t(vsRow_[k])]; break;
        default: i = elCur_[k]; break;
    }
    return pin == 0 ? i : (pin == 1 ? -i : 0.0);
}

double MnaSolver::elementPower(int k) const noexcept {
    if (k < 0 || k >= int(el_.size())) return 0.0;
    int node[4];
    const int np = pins(std::size_t(k), node);
    double p = 0;
    for (int q = 0; q < np; ++q) p += voltage(node[q]) * pinCurrent(k, q);
    return p;
}

double MnaSolver::elementState(int k) const noexcept {
    if (k < 0 || k >= int(el_.size()) || capSlot_[k] < 0) return 0.0;
    return qOld_[capSlot_[k]];
}

void MnaSolver::getState(double* out) const noexcept {
    std::copy(x_.begin(), x_.end(), out);
    std::copy(qOld_.begin(), qOld_.end(), out + m_);
}

bool MnaSolver::isStateValid(const double* in) const noexcept {
    if (!in) return false;
    for (std::size_t k = 0; k < qOld_.size(); ++k)
        if (!std::isfinite(in[m_ + k])) return false;
    return finiteSolution(in, in + m_);
}

bool MnaSolver::setState(const double* in) noexcept {
    if (!isStateValid(in)) return false;
    std::copy(in, in + m_, x_.begin());
    std::copy(in + m_, in + m_ + qOld_.size(), qOld_.begin());
    return true;
}

bool MnaSolver::finiteSolution(const double* x, const double* storedState) const noexcept {
    // Unknowns no larger than half the double range cannot overflow a terminal difference, so the
    // per-element difference check is only needed beyond that bound (rare; same result).
    constexpr double kHalfMax = 0.5 * std::numeric_limits<double>::max();
    bool large = false;
    for (int k = 0; k < m_; ++k) {
        if (!std::isfinite(x[k])) return false;
        large = large || std::fabs(x[k]) > kHalfMax;
    }
    auto diff = [&](const NetElement& e) { return (e.a ? x[e.a - 1] : 0.0) - (e.b ? x[e.b - 1] : 0.0); };
    if (large)
        for (const auto& e : el_) if (!std::isfinite(diff(e))) return false;
    for (int k : stateful_) {
        const auto& e = el_[std::size_t(k)];
        const double v = diff(e);
        switch (cat_[std::size_t(k)]) {
            case Cat::Cap: if (!std::isfinite(e.param[0] * v)) return false; break;
            case Cat::Ind: {
                const auto stateIndex = capSlot_[std::size_t(k)];
                const double q = storedState ? storedState[stateIndex] : qOld_[stateIndex];
                if (!std::isfinite(q + h_ * v)) return false;
                break;
            }
            default: if (!std::isfinite(v / e.param[1])) return false; break;  // electrolytic
        }
    }
    return true;
}

}  // namespace cd::dsp
