// CircuitDrift dsp: modified nodal analysis with damped Newton (P02/P03, 01-runtime contract).
// Backward Euler: (q(x)-q_old)/h + f(x) - u = 0. Analytic Jacobians from the manifest equation
// family, scaled partial-pivot LU, previous solution as initial guess, capped Newton iterations,
// four damped trials per iteration, accept when scaled residual and relative step <= tol.
// Validated only against labelled synthetic fixtures; no physical device claim (G01).
//
// Element families: resistor, capacitor, Shockley diode (P02); electrolytic (ESR + C||leakage,
// internal capacitor-voltage state), saturating inductor (flux-linkage state), Koren triode and
// pentode, Ebers-Moll NPN and square-law N-JFET (P03, 3/4-terminal). Linear stamps (conductances
// and voltage-source incidence) are cached per substep size/parameter set; Newton iterations only
// re-stamp nonlinear elements. Results are identical in exact arithmetic to full re-stamping.
//
// Threading: prepare() allocates and runs the DC solve off-thread; step()/setSource()/
// setElementParam()/setStepSize()/state access are noexcept and allocation-free.
#pragma once
#include <string>
#include <vector>
#include "dsp/DeviceManifest.h"

namespace cd::dsp {

struct NetElement {
    enum class Kind : unsigned char { Device, VoltageSource, CurrentSource };
    static constexpr int kMaxParams = 8, kMaxPins = 4;
    Kind kind = Kind::Device;
    const DeviceManifest* model = nullptr;  // Device only
    int a = 0, b = 0;                       // node indices, 0 = ground; Device: pins[0], pins[1]
    double param[kMaxParams] = {};          // Device: manifest parameter values in manifest order
    double value = 0;                       // source volts (a=+) / amps (flowing a -> b through source)
    int c = 0, d = 0;                       // Device pins[2], pins[3] (3/4-terminal families only)
    bool internalPortSource = false;        // compiler-generated source, from the separate port budget
};

struct Netlist {
    int nodes = 0;                          // non-ground nodes, numbered 1..nodes
    std::vector<NetElement> elements;
    double voltageScaleV = 1, currentScaleA = 1e-3;  // circuit scales, strictly positive
};

class MnaSolver {
public:
    static constexpr int kMaxNodes = 32, kMaxUnknowns = 96, kMaxComponents = 64, kMaxNonlinear = 8;
    static constexpr int kMaxPortSources = 4, kMaxElements = kMaxComponents + kMaxPortSources;
    static constexpr int kDcIterations = 100;
    struct Options { int maxNewton = 8; double tol = 1e-7; }; // finite 0 < tol <= 1e-7

    // Validates topology/bounds, allocates, solves the DC operating point (capacitors open,
    // inductors at winding resistance; source stepping as a bounded fallback) and initialises
    // finite element states from it. Returns false with a reason without changing the previously
    // prepared solver. A fresh solver remains unprepared on failure.
    bool prepare(const Netlist& net, double h, const Options& opt, std::string& error);
    // One backward-Euler substep at the current source values. On failure the last valid state is
    // kept (never committed invalid), the failure counted, and false returned.
    bool step() noexcept;
    void setSource(int element, double value) noexcept;

    // Runtime parameter change (audio-thread safe, bounded). The value is clamped to the
    // manifest bounds; non-finite values or bad indices are ignored (false). A capacitor value
    // change preserves the terminal voltage (stored charge rescaled); electrolytic and inductor
    // changes preserve their capacitor-voltage / flux state. Linear stamps are refreshed lazily.
    bool setElementParam(int element, int index, double value) noexcept;
    double elementParam(int element, int index) const noexcept;
    // New substep size (quality change). States are h-independent (charge, voltage, flux).
    bool setStepSize(double h) noexcept;
    double stepSize() const noexcept { return h_; }
    void setMaxNewton(int n) noexcept;      // clamped to 1..12
    int maxNewton() const noexcept { return maxNewton_; }
    void resetFailureCount() noexcept { consecutive_ = 0; }
    // Re-solve the DC operating point from the current state with the current element values and
    // sources (bounded: 100 Newton iterations, then 10-step source stepping), re-initialising
    // element states. Intended for preparation code; false leaves the state unchanged.
    bool solveOperatingPoint() noexcept;

    int unknowns() const noexcept { return m_; }
    int elementCount() const noexcept { return int(el_.size()); }
    double voltage(int node) const noexcept { return node <= 0 ? 0.0 : x_[node - 1]; }
    double unknown(int k) const noexcept { return x_[k]; }
    int lastIterations() const noexcept { return lastIter_; }
    long failures() const noexcept { return failures_; }
    bool needsFallback() const noexcept { return consecutive_ >= 3; }
    double lastScaledResidual() const noexcept { return lastRes_; }
    // Diagnostic of the most recent failed solve (preserved for the error report, 01-runtime).
    enum class FailReason : unsigned char { None, NonFiniteResidual, SingularJacobian, NonFiniteStep, FinalResidual, NonFiniteTrial, IterationCap };
    FailReason lastFailure() const noexcept { return lastFailure_; }

    // Committed readouts (last successful step or DC solve). pinCurrent: current flowing from the
    // pin's node into the element (2-terminal: pin 0 = a->b current). elementPower: sum of
    // V_pin*I_pin, the electrical power absorbed. elementState: charge [C] (capacitor), internal
    // capacitor voltage [V] (electrolytic), flux linkage [Wb] (inductor), else 0.
    double pinCurrent(int element, int pin) const noexcept;
    double elementPower(int element) const noexcept;
    double elementState(int element) const noexcept;

    // Residual/Jacobian at arbitrary x for the current step context (tests, diagnostics).
    // dc=true omits charge terms. J is row-major m x m.
    void evaluate(const double* x, double* F, double* J, bool dc) noexcept;

    // Dynamic state for checkpoints: unknowns followed by stored element states.
    // Buffers must hold stateSize() doubles. setState ignores a null buffer, non-finite values,
    // or voltages whose terminal differences/states overflow, leaving the state unchanged.
    int stateSize() const noexcept { return m_ + int(qOld_.size()); }
    void getState(double* out) const noexcept;
    bool isStateValid(const double* in) const noexcept;
    bool setState(const double* in) noexcept;

private:
    enum class Cat : unsigned char { Res, Cap, Elec, Ind, Diode, Triode, Pentode, Npn, Njfet, VSrc, ISrc };
    bool prepareInternal(const Netlist& net, double h, const Options& opt, std::string& error);
    bool newton(int maxIter, bool dc) noexcept;
    bool luSolve(double* A, double* b) noexcept;
    double scaledResidual(const double* F) const noexcept;
    bool finiteSolution(const double* x, const double* storedState = nullptr) const noexcept;
    void evalDc(const double* x, double* F, double* J) noexcept;
    void evalFast(const double* x, double* F, double* J) noexcept;
    void stampNonlinear(const double* x, double* F, double* J, bool dc) noexcept;
    void refreshLinear() noexcept;
    void buildRhs() noexcept;
    void commitStep() noexcept;
    bool initDcStates() noexcept;
    int pins(std::size_t k, int* node) const noexcept;
    double inductorFlux(std::size_t k, double v) const noexcept;  // committed-state helper

    std::vector<NetElement> el_;
    std::vector<Cat> cat_;
    std::vector<int> vsRow_;            // per element: branch unknown index or -1
    std::vector<int> capSlot_;          // per element: qOld_ index or -1 (all stateful elements)
    std::vector<int> nonlinear_;        // indices of nonlinear elements
    std::vector<int> stateful_;         // indices of elements with stored state
    std::vector<int> rhsElems_;         // indices of elements with source/history RHS terms
    std::vector<double> qOld_, x_, xSave_, xTry_, F_, Ftry_, J_, dx_, rowScale_, unitScale_;
    std::vector<double> Jtry_, Jlin_, rhs_, elG_, elD_, elCur_, devI_;
    int n_ = 0, m_ = 0, maxNewton_ = 8, lastIter_ = 0, consecutive_ = 0;
    long failures_ = 0;
    double h_ = 0, tol_ = 1e-7, vScale_ = 1, iScale_ = 1e-3, lastRes_ = 0, srcScale_ = 1;
    bool linDirty_ = true;
    FailReason lastFailure_ = FailReason::None;
};

}  // namespace cd::dsp
