// CircuitDrift dsp: modified nodal analysis with damped Newton (P02/P03, 01-runtime contract).
// Backward Euler: (q(x)-q_old)/h + f(x) - u = 0. Analytic Jacobians from the manifest equation
// family, scaled partial-pivot LU, previous solution as initial guess, capped Newton iterations,
// four damped trials per iteration, accept when scaled residual and relative step <= tol.
// Validated only against labelled synthetic RC/diode fixtures; no physical device claim (G01).
// Threading: prepare() allocates and runs the DC solve off-thread; step()/setSource()/state access
// are noexcept and allocation-free.
#pragma once
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
    static constexpr int kDcIterations = 100;
    struct Options { int maxNewton = 8; double tol = 1e-7; };

    // Validates topology/bounds, allocates, solves the DC operating point (capacitors open) and
    // initialises charges from it. Returns false with a reason; solver is then unusable.
    bool prepare(const Netlist& net, double h, const Options& opt, std::string& error);
    // One backward-Euler substep at the current source values. On failure the last valid state is
    // kept (never committed invalid), the failure counted, and false returned.
    bool step() noexcept;
    void setSource(int element, double value) noexcept;

    int unknowns() const noexcept { return m_; }
    double voltage(int node) const noexcept { return node <= 0 ? 0.0 : x_[node - 1]; }
    double unknown(int k) const noexcept { return x_[k]; }
    int lastIterations() const noexcept { return lastIter_; }
    long failures() const noexcept { return failures_; }
    bool needsFallback() const noexcept { return consecutive_ >= 3; }
    double lastScaledResidual() const noexcept { return lastRes_; }

    // Residual/Jacobian at arbitrary x for the current step context (tests, diagnostics).
    // dc=true omits charge terms. J is row-major m x m.
    void evaluate(const double* x, double* F, double* J, bool dc) noexcept;

    // Dynamic state for checkpoints: unknowns followed by stored capacitor charges.
    int stateSize() const noexcept { return m_ + int(qOld_.size()); }
    void getState(double* out) const noexcept;
    void setState(const double* in) noexcept;

private:
    bool newton(int maxIter, bool dc) noexcept;
    bool luSolve(double* A, double* b) noexcept;
    double scaledResidual(const double* F) const noexcept;

    std::vector<NetElement> el_;
    std::vector<int> vsRow_;            // per element: branch unknown index or -1
    std::vector<int> capSlot_;          // per element: qOld_ index or -1
    std::vector<double> qOld_, x_, xSave_, xTry_, F_, Ftry_, J_, dx_, rowScale_, unitScale_;
    int n_ = 0, m_ = 0, maxNewton_ = 8, lastIter_ = 0, consecutive_ = 0;
    long failures_ = 0;
    double h_ = 0, tol_ = 1e-7, vScale_ = 1, iScale_ = 1e-3, lastRes_ = 0;
};

}  // namespace cd::dsp
