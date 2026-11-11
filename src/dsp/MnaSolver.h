// CircuitDrift dsp: modified nodal analysis with damped Newton (P02/P03, 01-runtime contract).
// Backward Euler: (q(x)-q_old)/h + f(x) - u = 0. Analytic Jacobians from the manifest equation
// family, scaled partial-pivot LU, previous solution as initial guess, capped Newton iterations,
// four damped trials per iteration, accept when scaled residual and relative step <= tol.
// Validated only against labelled synthetic RC/diode fixtures; no physical device claim (G01).
// Threading: prepare() allocates and runs the DC solve off-thread; step()/setSource()/state access
// are noexcept and allocation-free.
// Additive parameter-update API (temperature feedback, resume packet 2): setElementParameter()
// changes one manifest parameter of one Device element between substeps. It is noexcept, bounded
// (O(1)) and allocation-free, and validates the domain exactly like prepare() (finite, inside the
// manifest's [min,max], numerically representable conductance/step conductance/diode nVt). A
// rejected update leaves every value unchanged. Capacitor updates keep the stored charge q_old
// (charge conservation: the next step sees q_old with the new C). The DC operating point is not
// re-solved; the next step() starts from the previous solution as usual.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "dsp/DeviceManifest.h"

namespace cd::dsp {

struct NetElement {
    enum class Kind : unsigned char { Device, VoltageSource, CurrentSource };
    Kind kind = Kind::Device;
    const DeviceManifest* model = nullptr;  // Device only
    int a = 0, b = 0;                       // node indices, 0 = ground; Device: pins[0], pins[1]
    double param[3] = {0, 0, 0};            // Device: manifest parameter values in manifest order
    double value = 0;                       // source volts (a=+) / amps (flowing a -> b through source)
};

struct Netlist {
    int nodes = 0;                          // non-ground nodes, numbered 1..nodes
    std::vector<NetElement> elements;
    double voltageScaleV = 1, currentScaleA = 1e-3;  // circuit scales, strictly positive
};

class MnaSolver {
public:
    static constexpr int kMaxNodes = 32, kMaxUnknowns = 96, kMaxComponents = 64, kMaxNonlinear = 8;
    // Port drive sources (input/supply ports) are injected by the graph compiler in addition to the
    // 64 user components of the blueprint bound; the solver element bound covers both.
    static constexpr int kMaxPortSources = 4, kMaxElements = kMaxComponents + kMaxPortSources;
    static constexpr int kDcIterations = 100;
    struct Options { int maxNewton = 8; double tol = 1e-7; }; // finite 0 < tol <= 1e-7

    // Validates topology/bounds, allocates, solves the DC operating point (capacitors open) and
    // initialises finite charges from it. Returns false with a reason without changing the
    // previously prepared solver. A fresh solver remains unprepared on failure.
    bool prepare(const Netlist& net, double h, const Options& opt, std::string& error);
    // One backward-Euler substep at the current source values. On failure the last valid state is
    // kept (never committed invalid), the failure counted, and false returned.
    bool step() noexcept;
    void setSource(int element, double value) noexcept;
    // Additive (see header): false and no change for a non-Device element, an out-of-range index,
    // or a value outside the manifest domain / numeric range.
    bool setElementParameter(int element, int paramIndex, double value) noexcept;
    // Current value of a Device element parameter; NaN for an invalid element/index.
    double elementParameter(int element, int paramIndex) const noexcept;
    int elementCount() const noexcept { return int(el_.size()); }

    int unknowns() const noexcept { return m_; }
    double voltage(int node) const noexcept { return node <= 0 ? 0.0 : x_[std::size_t(node)]; }
    double unknown(int k) const noexcept { return x_[std::size_t(k) + 1]; }
    int lastIterations() const noexcept { return lastIter_; }
    std::int64_t failures() const noexcept { return failures_; }
    bool needsFallback() const noexcept { return consecutive_ >= 3; }
    double lastScaledResidual() const noexcept { return lastRes_; }

    // Residual/Jacobian at arbitrary x for the current step context (tests, diagnostics).
    // dc=true omits charge terms. J is row-major m x m.
    void evaluate(const double* x, double* F, double* J, bool dc) noexcept;

    // Dynamic state for checkpoints: unknowns followed by stored capacitor charges.
    // Buffers must hold stateSize() doubles. setState ignores a null buffer, non-finite values,
    // or voltages whose terminal differences/charges overflow, leaving the state unchanged.
    int stateSize() const noexcept { return m_ + int(qOld_.size()); }
    void getState(double* out) const noexcept;
    void setState(const double* in) noexcept;

private:
    // Performance layout (bit-identical to the original element loop): unknown k lives at padded
    // index k+1 and padded index 0 is ground (always 0 in x buffers; a dump slot in F/J), so stamps
    // need no ground branches. Per-element constants (1/R, C/h, n*Vt) are cached and refreshed by
    // setElementParameter(). Element order, operation order and rounding are unchanged.
    struct Op {
        enum Kind : unsigned char { Res, Cap, Diode, ISrc, VSrc };
        Kind kind = Res;
        int a = 0, b = 0;       // padded indices (node numbers; 0 = ground)
        int row = 0;            // VSrc: padded branch row
        int slot = -1;          // Cap: charge slot
        int elem = 0;           // index into el_
        double p0 = 0;          // Res: 1/R; Cap: C; Diode: Is
        double c1 = 0;          // Cap: C/h; Diode: n*Vt
        double cap = 0;         // Diode: maxExpArg
    };
    bool prepareInternal(const Netlist& net, double h, const Options& opt, std::string& error);
    void buildOps() noexcept;
    void refreshOp(int element) noexcept;
    bool newton(int maxIter, bool dc) noexcept;
    bool luSolve(double* A, int ld, double* b) noexcept;
    void evalPadded(const double* xp, double* Fp, double* Jp, bool dc) const noexcept;
    double scaledResidual(const double* Fp) const noexcept;   // Fp padded (index k+1)
    bool finiteSolution(const double* xp) const noexcept;     // xp padded
    void storeCharges() noexcept;

    std::vector<NetElement> el_;
    std::vector<Op> ops_;
    std::vector<int> vsRow_;            // per element: branch unknown index or -1
    std::vector<int> capSlot_;          // per element: qOld_ index or -1
    // x_, xSave_, xTry_, F_, Ftry_ are padded (m_+1); J_, Jtry_ are (m_+1)^2 row-major with ld m_+1.
    std::vector<double> qOld_, x_, xSave_, xTry_, F_, Ftry_, J_, Jtry_, dx_, rowScale_, unitScale_;
    std::vector<double> xEval_, FEval_, JEval_;   // scratch for the public evaluate()
    int n_ = 0, m_ = 0, maxNewton_ = 8, lastIter_ = 0, consecutive_ = 0;
    std::int64_t failures_ = 0;
    double h_ = 0, tol_ = 1e-7, vScale_ = 1, iScale_ = 1e-3, lastRes_ = 0;
};

}  // namespace cd::dsp
