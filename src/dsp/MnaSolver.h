// CircuitDrift dsp: modified nodal analysis with damped Newton (P02/P03, 01-runtime contract).
// Backward Euler: (q(x)-q_old)/h + f(x) - u = 0. Analytic Jacobians from the manifest equation
// family, scaled partial-pivot LU, previous solution as initial guess, capped Newton iterations,
// four damped trials per iteration, accept when scaled residual and relative step <= tol.
// Validated only against labelled synthetic RC/diode fixtures; no physical device claim (G01).
// Threading: prepare() allocates and runs the DC solve off-thread; step()/setSource()/
// updateParameter()/state access are noexcept and allocation-free by construction (they only
// write into storage sized at prepare).
// Performance (P17): prepare() also compiles the netlist into a flat per-element stamp table
// (ground-resolved indices, cached conductances); the LU is specialised for m <= 8; the Jacobian
// evaluated at an accepted damped trial is reused by the next Newton iteration (same point, same
// step context: still a true Newton); diode exponentials are reused when the argument is
// bit-identical. All of these keep the arithmetic, and therefore the results, bit-identical.
#pragma once
#include <cstdint>
#include <limits>
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
    // Bounded device-parameter update (prerequisite for temperature feedback, G01). element indexes
    // the prepared netlist, k the manifest parameter (manifest order). Accepts only a Device element,
    // a valid k, a finite value inside the manifest [min, max] and the same numeric-domain rules as
    // prepare() (finite 1/R, finite C/h and C*v at the current state, finite positive n*Vt).
    // Takes effect at the next step(); no re-prepare, no allocation. It changes neither topology nor
    // the DC operating point / dcState held by callers, and stored capacitor charges are kept: the
    // caller decides whether to reset. Returns false and leaves all state untouched otherwise.
    bool updateParameter(int element, int k, double value) noexcept;
    // Current value of parameter k of a Device element; NaN when element/k is invalid.
    double parameter(int element, int k) const noexcept;

    int unknowns() const noexcept { return m_; }
    double voltage(int node) const noexcept { return node <= 0 ? 0.0 : x_[node - 1]; }
    double unknown(int k) const noexcept { return x_[k]; }
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
    bool prepareInternal(const Netlist& net, double h, const Options& opt, std::string& error);
    bool newton(int maxIter, bool dc) noexcept;
    bool finiteSolution(const double* x) const noexcept;
    // Size-specialised kernels: M = m_ for m_ <= 8, M = 0 for the runtime-size fallback.
    template <int M> bool newtonT(int maxIter, bool dc) noexcept;
    template <int M> void evalT(const double* x, double* F, double* J, bool dc) noexcept;
    template <int M> double resT(const double* F) const noexcept;
    template <int M> bool finiteT(const double* x) const noexcept;
    void storeCharges() noexcept;
    void compile(int k) noexcept;
    void refreshCapMax() noexcept;  // capMax_ = largest capacitance (finiteSolution shortcut)   // refresh the stamp entry of element k from el_[k]
    template <int M> bool luFixed(double* A, double* b, int mRuntime) noexcept;

    // Flat stamp table (one per element), built at prepare and refreshed by updateParameter.
    struct Stamp {
        enum Kind : unsigned char { Resistor, Capacitor, Diode, CurrentSource, VoltageSource };
        Kind kind = Resistor;
        int ia = -1, ib = -1;           // unknown index of node a/b, -1 = ground
        int aux = -1;                   // capacitor: qOld_ slot; voltage source: branch row
        double g = 0;                   // resistor: 1/R; capacitor: C/h
        double p0 = 0;                  // resistor R, capacitor C, diode Is
        double nvt = 1, cap = 40, expCap = 0;   // diode: n*Vt, exponent cap, exp(cap)
        double lastArg = std::numeric_limits<double>::quiet_NaN(), lastExp = 0;     // diode: exp cache (reused only on identical arg)
        double value = 0;               // source value
    };

    std::vector<NetElement> el_;
    std::vector<Stamp> st_;
    std::vector<int> vsRow_;            // per element: branch unknown index or -1
    std::vector<int> capSlot_;          // per element: qOld_ index or -1
    std::vector<double> qOld_, x_, xSave_, xTry_, F_, Ftry_, J_, dx_, rowScale_, unitScale_;
    int n_ = 0, m_ = 0, maxNewton_ = 8, lastIter_ = 0, consecutive_ = 0;
    std::int64_t failures_ = 0;
    double h_ = 0, tol_ = 1e-7, vScale_ = 1, iScale_ = 1e-3, lastRes_ = 0, capMax_ = 0;
};

}  // namespace cd::dsp
