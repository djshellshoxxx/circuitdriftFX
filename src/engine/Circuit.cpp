#include "engine/Circuit.h"
#include <algorithm>
#include <cmath>

namespace cd::engine {
namespace {

// SYNTHETIC FIXTURE DESIGNS (G01): round illustrative values chosen for plausible operating
// points (12AX7-like plate ~185 V / ~0.9 mA; EF86-like screen ~130 V; NPN ~1.3 mA; N-JFET
// ~0.75 mA). They are not copied from, fitted to or measured on any real unit.
// Input staging (inVoltsPerFs) places 0 dBFS at the maximum 36 dB drive in hard but bounded
// overdrive (triode ~16 V, pentode ~3.8 V, NPN ~0.3 V, N-JFET ~3.2 V at the stage input) so the
// Studio Newton cap converges; the source compliance (sourceCompliance) bounds extra input gain.
const FamilyDesign kDesigns[kFamilyCount] = {
    {Family::Triode12AX7, "synthetic.triode-12ax7", "synthetic.film-capacitor", 600,
     {22e-9, 0.1, 1e12}, {22e-6, 1.5, 5e6}, {1e-6, 3.0, 1e8}, {22e-6, 2.0, 1e7}, {0, 0, 0},
     {1e6, 2e3, 5e4, 5e3, 1e5},
     1e6, 0, 1e5, 1500, 0, 1e4, 0,
     600, 0.4, 4e5, 1e5, 300, 1.0 / 13.0,
     280, 0.0224, 25,
     0.25, 1.0, 1.9,
     100, 1e-3,
     {100, 1.4, 1060, 300, 300, 5e-4, 0.1, 0}, 7},
    {Family::PentodeEF86, "synthetic.pentode-ef86", "synthetic.film-capacitor", 600,
     {22e-9, 0.1, 1e12}, {22e-6, 1.5, 5e6}, {1e-6, 3.0, 1e8}, {22e-6, 2.0, 1e7}, {4.7e-7, 0.5, 1e10},
     {1e6, 2e3, 5e4, 5e3, 1e5},
     1e6, 0, 1e5, 2200, 0, 1e4, 4.7e5,
     600, 0.4, 4e5, 1e5, 300, 1.0 / 13.0,
     280, 0.0224, 25,
     0.06, 1.5, 1.26,
     100, 1e-3,
     {38, 1.35, 2000, 9000, 75, 15, 5e-4, 0.1}, 8},
    {Family::Bjt, "synthetic.npn-small-signal", "synthetic.electrolytic", 600,
     {10e-6, 2.0, 1e8}, {47e-6, 0.5, 2e6}, {10e-6, 2.0, 1e8}, {100e-6, 1.0, 1e7}, {0, 0, 0},
     {1e4, 500, 1e4, 1e3, 1e4},
     15e3, 82e3, 10e3, 2700, 220, 1000, 0,
     80, 0.04, 4e4, 1e4, 30, 0.5,
     30, 0.030, 25,
     0.005, 0.15, 0,
     10, 1e-3,
     {1e-14, 200, 2, 0.025693, 0, 0, 0, 0}, 4},
    {Family::Jfet, "synthetic.njfet-small-signal", "synthetic.film-capacitor", 600,
     {100e-9, 0.1, 1e12}, {47e-6, 0.5, 2e6}, {10e-6, 2.0, 1e8}, {100e-6, 1.0, 1e7}, {0, 0, 0},
     {1e6, 500, 1e4, 1e3, 1e4},
     1e6, 0, 12e3, 1000, 0, 1000, 0,
     80, 0.04, 4e4, 1e4, 30, 0.5,
     30, 0.030, 25,
     0.05, 0.5, 0,
     10, 1e-3,
     {5e-4, 2.0, 0.01, 0.05, 1e-12, 0.025693, 0, 0}, 6},
};

// Synthetic core materials: flux scale (saturation onset), knee exponent, permeability and
// core-loss resistance scales relative to the Steel reference. Not measured lamination data.
const CoreMaterial kCores[3] = {
    {"Nickel", 0.6, 6.0, 2.0, 2.0, 0},
    {"Steel", 1.0, 4.0, 1.0, 1.0, 0},
    {"Iron", 0.8, 2.0, 0.7, 0.5, 0},
};

dsp::NetElement device(const char* model, int a, int b, std::initializer_list<double> p) {
    dsp::NetElement e;
    e.model = dsp::findSyntheticDevice(model);
    e.a = a; e.b = b;
    int k = 0;
    for (double v : p) e.param[k++] = v;
    return e;
}
dsp::NetElement current(int into) {
    dsp::NetElement e;
    e.kind = dsp::NetElement::Kind::CurrentSource;
    e.a = 0; e.b = into;
    return e;
}
double capLoss(const dsp::MnaSolver& s, int k) noexcept {
    if (k < 0) return 0.0;
    const double i = s.pinCurrent(k, 0), vc = s.elementState(k);
    return s.elementParam(k, 1) * i * i + vc * vc / s.elementParam(k, 2);
}
}  // namespace

const FamilyDesign& familyDesign(Family f) noexcept { return kDesigns[std::clamp(int(f), 0, kFamilyCount - 1)]; }
const CoreMaterial& coreMaterial(int i) noexcept { return kCores[std::clamp(i, 0, 2)]; }

CircuitValues nominalValues(Family f, int core) noexcept {
    const FamilyDesign& d = familyDesign(f);
    const CoreMaterial& m = coreMaterial(core);
    CircuitValues v;
    v.rG = d.rG; v.rTop = d.rTop; v.rLoad = d.rLoad; v.rK = d.rK; v.rDec = d.rDec; v.rScreen = d.rScreen;
    v.rCore = d.rCore * m.lossScale; v.rRefl = d.rRefl; v.rWind = d.rWind;
    v.cIn = d.cIn; v.cK = d.cK; v.cK.esr += d.kDegeneration; v.cOut = d.cOut; v.cDec = d.cDec; v.cScreen = d.cScreen;
    v.lM = d.lM * m.inductanceScale; v.lambdaS = d.lambdaS * m.fluxScale; v.exponent = m.exponent;
    std::copy(d.dev, d.dev + 8, v.dev);
    return v;
}

bool CircuitPath::build(Family f, const CircuitValues& v, double h, int maxNewton, double vRes, std::string& err) {
    const FamilyDesign& d = familyDesign(f);
    dsp::Netlist n;
    const bool pentode = f == Family::PentodeEF86;
    n.nodes = pentode ? 6 : 5;
    n.voltageScaleV = d.voltageScaleV;
    n.currentScaleA = d.currentScaleA;
    for (int& k : idx_) k = -1;
    auto add = [&](CircuitElem id, const dsp::NetElement& e) { idx_[id] = int(n.elements.size()); n.elements.push_back(e); };
    add(eInSrc, current(kNodeIn));
    add(eRsrc, device("fixture.resistor", kNodeIn, 0, {d.rSrc}));
    add(eCin, device(d.cInModelId, kNodeIn, kNodeCtrl, {v.cIn.c, v.cIn.esr, v.cIn.leak}));
    add(eRg, device("fixture.resistor", kNodeCtrl, 0, {v.rG}));
    add(eBiasSrc, current(kNodeCtrl));
    dsp::NetElement dev;
    dev.model = dsp::findSyntheticDevice(d.deviceModelId);
    dev.a = kNodeCtrl; dev.b = kNodeOut; dev.c = kNodeCom; dev.d = pentode ? kNodeScreen : 0;
    std::copy(v.dev, v.dev + 8, dev.param);
    add(eDev, dev);
    add(eRload, device("fixture.resistor", kNodeOut, 0, {v.rLoad}));
    add(eLoadSrc, current(kNodeOut));
    add(eRk, device("fixture.resistor", kNodeCom, 0, {v.rK}));
    add(eCk, device("synthetic.electrolytic", kNodeCom, 0, {v.cK.c, v.cK.esr, v.cK.leak}));
    add(eCout, device("synthetic.electrolytic", kNodeOut, kNodeXp, {v.cOut.c, v.cOut.esr, v.cOut.leak}));
    add(eTout, device("synthetic.output-transformer", kNodeXp, 0, {v.lM, v.lambdaS, v.exponent, v.rWind}));
    add(eRcore, device("fixture.resistor", kNodeXp, 0, {v.rCore}));
    add(eRrefl, device("fixture.resistor", kNodeXp, 0, {v.rRefl}));
    if (v.rTop > 0) {
        add(eRtop, device("fixture.resistor", kNodeCtrl, 0, {v.rTop}));
        add(eTopSrc, current(kNodeCtrl));
    }
    if (pentode) {
        add(eRscreen, device("fixture.resistor", kNodeScreen, 0, {v.rScreen}));
        add(eScreenSrc, current(kNodeScreen));
        add(eCscreen, device("synthetic.electrolytic", kNodeScreen, 0, {v.cScreen.c, v.cScreen.esr, v.cScreen.leak}));
    }
    for (const auto& e : n.elements)
        if (e.kind == dsp::NetElement::Kind::Device && !e.model) { err = "circuit: missing synthetic manifest"; return false; }
    fam_ = f;
    h_ = h;
    gSrc_ = 1.0 / d.rSrc;
    vRes_ = vRes;
    vB_ = vRes;
    gDec_ = 1.0 / v.rDec;
    gLoad_ = 1.0 / v.rLoad;
    gTop_ = v.rTop > 0 ? 1.0 / v.rTop : 0.0;
    gScr_ = pentode ? 1.0 / v.rScreen : 0.0;
    auto loads = [&] {
        n.elements[std::size_t(idx_[eLoadSrc])].value = vB_ * gLoad_;
        if (idx_[eTopSrc] >= 0) n.elements[std::size_t(idx_[eTopSrc])].value = vB_ * gTop_;
        if (idx_[eScreenSrc] >= 0) n.elements[std::size_t(idx_[eScreenSrc])].value = vB_ * gScr_;
    };
    loads();
    if (!s_.prepare(n, h, {std::clamp(maxNewton, 1, 12), 1e-7}, err)) { err = "circuit DC: " + err; return false; }
    built_ = true;
    apply(v);
    // DC fixed point with the explicit local B+ node: V_B = (V_res/R_dec - I_stage) / (1/R_dec + G_leak).
    for (int it = 0; it < 40; ++it) {
        const double v3 = s_.voltage(kNodeOut);
        double i = (vB_ - v3) * gLoad_;
        if (gTop_ > 0) i += (vB_ - s_.voltage(kNodeCtrl)) * gTop_;
        if (gScr_ > 0) i += (vB_ - s_.voltage(kNodeScreen)) * gScr_;
        const double vNew = (vRes_ * gDec_ - i) / (gDec_ + gDecLeak_);
        const bool done = std::fabs(vNew - vB_) <= 1e-10 * std::max(1.0, vRes_);
        vB_ = vNew;
        refreshLoads();
        if (!s_.solveOperatingPoint()) { err = "circuit DC: local B+ operating point did not converge"; built_ = false; return false; }
        if (done) break;
    }
    return true;
}

bool CircuitPath::setStepSize(double h) noexcept {
    if (!s_.setStepSize(h)) return false;
    h_ = h;
    cDecH_ = cDec_ / h;
    return true;
}

void CircuitPath::refreshLoads() noexcept {
    s_.setSource(idx_[eLoadSrc], vB_ * gLoad_);
    if (idx_[eTopSrc] >= 0) s_.setSource(idx_[eTopSrc], vB_ * gTop_);
    if (idx_[eScreenSrc] >= 0) s_.setSource(idx_[eScreenSrc], vB_ * gScr_);
}

bool CircuitPath::step() noexcept {
    const bool ok = s_.step();
    // Local B+ (stiff explicit state): backward Euler with the stage current of this substep.
    double i = (vB_ - s_.voltage(kNodeOut)) * gLoad_;
    if (gTop_ > 0) i += (vB_ - s_.voltage(kNodeCtrl)) * gTop_;
    if (gScr_ > 0) i += (vB_ - s_.voltage(kNodeScreen)) * gScr_;
    const double vNew = (cDecH_ * vB_ + vRes_ * gDec_ - i) / (cDecH_ + gDec_ + gDecLeak_);
    if (std::isfinite(vNew)) vB_ = vNew;
    refreshLoads();
    return ok;
}

void CircuitPath::apply(const CircuitValues& v) noexcept {
    auto set = [&](CircuitElem e, int i, double x) { if (idx_[e] >= 0) s_.setElementParam(idx_[e], i, x); };
    auto cap = [&](CircuitElem e, const CapValues& c) { set(e, 0, c.c); set(e, 1, c.esr); set(e, 2, c.leak); };
    set(eRg, 0, v.rG); set(eRtop, 0, v.rTop); set(eRload, 0, v.rLoad); set(eRk, 0, v.rK);
    set(eRscreen, 0, v.rScreen); set(eRcore, 0, v.rCore); set(eRrefl, 0, v.rRefl);
    cap(eCin, v.cIn); cap(eCk, v.cK); cap(eCout, v.cOut); cap(eCscreen, v.cScreen);
    set(eTout, 0, v.lM); set(eTout, 1, v.lambdaS); set(eTout, 2, v.exponent); set(eTout, 3, v.rWind);
    const int np = familyDesign(fam_).devParams;
    for (int i = 0; i < np; ++i) set(eDev, i, v.dev[i]);
    auto g = [&](CircuitElem e) { return idx_[e] >= 0 ? 1.0 / s_.elementParam(idx_[e], 0) : 0.0; };
    gLoad_ = g(eRload); gTop_ = g(eRtop); gScr_ = g(eRscreen); gK_ = g(eRk); gG_ = g(eRg); gCore_ = g(eRcore);
    rWind_ = s_.elementParam(idx_[eTout], 3);
    gDec_ = 1.0 / std::clamp(v.rDec, 1e-3, 1e9);
    cDec_ = std::max(v.cDec.c, 1e-12);
    gDecLeak_ = 1.0 / std::clamp(v.cDec.leak, 100.0, 1e13);
    cDecH_ = h_ > 0 ? cDec_ / h_ : 0.0;
    gBias_ = gG_ + gTop_;
    refreshLoads();
}

void CircuitPath::accumulatePower(double* p) const noexcept {
    const double v2 = s_.voltage(kNodeCtrl), v3 = s_.voltage(kNodeOut), v4 = s_.voltage(kNodeCom), v5 = s_.voltage(kNodeXp);
    p[int(ComponentId::V1)] += s_.elementPower(idx_[eDev]);
    p[int(ComponentId::R_load)] += (vB_ - v3) * (vB_ - v3) * gLoad_;
    p[int(ComponentId::R_k)] += v4 * v4 * gK_;
    p[int(ComponentId::R_g)] += v2 * v2 * gG_ + (vB_ - v2) * (vB_ - v2) * gTop_;
    p[int(ComponentId::C_in)] += capLoss(s_, idx_[eCin]);
    p[int(ComponentId::C_k)] += capLoss(s_, idx_[eCk]);
    p[int(ComponentId::C_out)] += capLoss(s_, idx_[eCout]);
    p[int(ComponentId::C_dec)] += vB_ * vB_ * gDecLeak_;
    const double im = s_.pinCurrent(idx_[eTout], 0);
    p[int(ComponentId::T_out)] += v5 * v5 * gCore_ + im * im * rWind_;
    const double idec = (vRes_ - vB_) * gDec_;
    p[int(ComponentId::R_dec)] += idec * idec / gDec_;
    if (gScr_ > 0) {
        const double v7 = s_.voltage(kNodeScreen);
        p[int(ComponentId::R_g2)] += (vB_ - v7) * (vB_ - v7) * gScr_;
        p[int(ComponentId::C_g2)] += capLoss(s_, idx_[eCscreen]);
    }
}

}  // namespace cd::engine
