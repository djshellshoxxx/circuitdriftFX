// CircuitDrift engine: factory circuit (one active gain stage per slot/channel) on dsp::MnaSolver.
//
// SYNTHETIC FIXTURE TOPOLOGY (G01 open; not calibrated hardware):
//   input Norton (v_in/R_src || R_src) -> C_in (film/electrolytic, ESR+leakage) -> control node with
//   grid leak / bias divider (+ bias current source) -> active element (Koren triode, Koren pentode,
//   Ebers-Moll NPN or square-law N-JFET) with load R_load to the local decoupled B+ node, cathode/
//   emitter R_k + bypass electrolytic C_k -> output coupling electrolytic C_out -> output transformer
//   magnetizing branch (flux-state saturating inductor) || core-loss R || reflected load.
//   Pentode only: screen grid fed from local B+ through R_g2 with bypass C_g2.
// Local B+ (C_dec, fed through R_dec from the slot supply filter node) is a stiff explicit state
// updated after every substep by backward Euler from the stage current solved in that substep;
// the stage sees it as Norton sources (V_B/R_load, V_B/R_top, V_B/R_g2): one-substep explicit
// coupling (C_dec/h >= 0.1 S vs. <= 1e-3 S stage conductance; documented). The slot supply filter
// node is updated once per host sample (one-host-sample lag).
// MNA size: 5 nodes (6 for the pentode), no voltage-source branches, 2 nonlinear elements.
#pragma once
#include <cmath>
#include <string>
#include "dsp/MnaSolver.h"
#include "engine/EngineTypes.h"

namespace cd::engine {

enum CircuitNode : int { kNodeIn = 1, kNodeCtrl, kNodeOut, kNodeCom, kNodeXp, kNodeScreen };
enum CircuitElem : int {
    eInSrc, eRsrc, eCin, eRg, eBiasSrc, eDev, eRload, eLoadSrc, eRk, eCk, eCout, eTout, eRcore, eRrefl,
    eRtop, eTopSrc, eRscreen, eScreenSrc, eCscreen, kElemCount
};

struct CapValues { double c = 0, esr = 0, leak = 0; };

// Synthetic family design (round illustrative values; G01).
struct FamilyDesign {
    Family family;
    const char* deviceModelId;
    const char* cInModelId;
    double rSrc;
    CapValues cIn, cK, cOut, cDec, cScreen;   // fresh values; leak = fresh leakage resistance
    double leakShort[5];                      // short-equivalent leakage bound: cIn, cK, cOut, cDec, cScreen
    double rG, rTop, rLoad, rK, kDegeneration, rDec, rScreen;
    double lM, lambdaS, rCore, rRefl, rWind, turnsRatio;   // Steel reference core
    double railDesignV, designA, auxGain;     // supply design rail V_d, design current I_d [A], downstream-load gain K
    double inVoltsPerFs, biasSpanV, heaterW;
    double voltageScaleV, currentScaleA;
    double dev[8];                            // nominal device parameters (manifest order)
    int devParams;
};
const FamilyDesign& familyDesign(Family f) noexcept;

// Transformer core materials (slot.*.transformer.core order): Nickel, Steel, Iron. Synthetic.
struct CoreMaterial { const char* name; double fluxScale, exponent, inductanceScale, lossScale, coerciveScale; };
const CoreMaterial& coreMaterial(int index) noexcept;

// Resolved element values for one path (tolerance, aging, temperature and condition applied).
struct CircuitValues {
    double rG = 0, rTop = 0, rLoad = 0, rK = 0, rDec = 0, rScreen = 0, rCore = 0, rRefl = 0, rWind = 0;
    CapValues cIn, cK, cOut, cDec, cScreen;
    double lM = 0, lambdaS = 0, exponent = 4;
    double dev[8] = {};
};
CircuitValues nominalValues(Family f, int core) noexcept;

// Input source compliance (SYNTHETIC): the source driving the stage saturates smoothly at
// +-V_c with V_c = 32 * inVoltsPerFs (30 dB above full scale at 0 dB drive). Applied per substep
// inside the oversampled region. Keeps extreme input gain within the stage's numeric domain.
inline double complianceVolts(const FamilyDesign& d) noexcept { return 32.0 * d.inVoltsPerFs; }
inline double sourceCompliance(double v, double vc) noexcept { return vc * std::tanh(v / vc); }

class CircuitPath {
public:
    // Off-thread: builds the netlist and solves the DC point (fixed point with the local B+ node)
    // at the given supply filter voltage. Allocates.
    bool build(Family f, const CircuitValues& v, double h, int maxNewton, double vRes, std::string& err);
    bool built() const noexcept { return built_; }
    Family family() const noexcept { return fam_; }
    // Audio thread.
    void apply(const CircuitValues& v) noexcept;              // element values (bounded by manifests)
    bool setStepSize(double h) noexcept;
    void setDrive(double vInVolts) noexcept { s_.setSource(idx_[eInSrc], vInVolts * gSrc_); }
    void setBias(double vBias) noexcept { s_.setSource(idx_[eBiasSrc], vBias * gBias_); }
    void setRail(double vRes) noexcept { vRes_ = vRes; }
    bool step() noexcept;
    double outputVolts() const noexcept { return s_.voltage(kNodeXp); }
    double nodeVolts(int n) const noexcept { return s_.voltage(n); }
    double localBplus() const noexcept { return vB_; }
    // Current drawn from the supply filter node through R_dec [A].
    double supplyCurrent() const noexcept { return (vRes_ - vB_) * gDec_; }
    // Dissipated power per component this substep [W] (added into p, indexed by ComponentId).
    void accumulatePower(double* p) const noexcept;
    double flux() const noexcept { return s_.elementState(idx_[eTout]); }
    double magnetizingCurrent() const noexcept { return s_.pinCurrent(idx_[eTout], 0); }
    double coreLossCurrent() const noexcept { return s_.voltage(kNodeXp) * gCore_; }
    dsp::MnaSolver& solver() noexcept { return s_; }
    const dsp::MnaSolver& solver() const noexcept { return s_; }
    double railVolts() const noexcept { return vRes_; }
    double& railRef() noexcept { return vRes_; }
    double& bplusRef() noexcept { return vB_; }
    void refreshLoads() noexcept;                             // Norton sources from V_B
private:
    dsp::MnaSolver s_;
    int idx_[kElemCount] = {};
    Family fam_ = Family::Triode12AX7;
    double gSrc_ = 0, gDec_ = 0, gBias_ = 0, vRes_ = 0, vB_ = 0;
    double gLoad_ = 0, gTop_ = 0, gScr_ = 0, gK_ = 0, gG_ = 0, gCore_ = 0, rWind_ = 0;
    double cDec_ = 0, gDecLeak_ = 0, cDecH_ = 0, h_ = 0;
    bool built_ = false;
};

}  // namespace cd::engine
