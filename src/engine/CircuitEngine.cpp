#include "engine/CircuitEngine.h"
#include "core/CutoffDomain.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include "dsp/OutputProtection.h"
#include "dsp/Oversampling.h"
#include "engine/Circuit.h"
#include "engine/Realtime.h"
#include "engine/Supply.h"
#include "engine/ThermalClock.h"

namespace cd::engine {
namespace {
using core::indexOf;
using core::kParamCount;

#define CD_P(name, id)                         \
    constexpr std::size_t name = indexOf(id); \
    static_assert(name < kParamCount, id)
CD_P(kPInGain, "input.gain");
CD_P(kPOutGain, "output.gain");
CD_P(kPMix, "mix");
CD_P(kPGainMatch, "gainMatch.enabled");
CD_P(kPSoftClip, "output.softClip");
CD_P(kPHpfOn, "hpf.enabled");
CD_P(kPHpfHz, "hpf.frequency");
CD_P(kPQualityMode, "quality.mode");
CD_P(kPOversampling, "quality.oversampling");
CD_P(kPPhase, "quality.phase");
CD_P(kPZeroLatency, "quality.zeroLatency");
CD_P(kPPitchDrift, "pitchDrift.enabled");
CD_P(kPAb, "ab.crossfade");
CD_P(kPAnalogNoise, "analogNoise.enabled");
#undef CD_P

struct SlotParams {
    std::size_t mains, railCap, rectifier, sag, age, tol, leak, resType, ambient, mass, drift, drive, element, bias,
        core, humHz, humLevel, jn, micro, lpfOn, lpfHz;
};
#define CD_SLOT(s)                                                                                          \
    SlotParams{indexOf("slot." s ".supply.mainsVoltage"), indexOf("slot." s ".supply.railCapacity"),         \
               indexOf("slot." s ".supply.rectifier"), indexOf("slot." s ".supply.sagSensitivity"),          \
               indexOf("slot." s ".condition.ageYears"), indexOf("slot." s ".condition.tolerance"),          \
               indexOf("slot." s ".condition.capLeakage"), indexOf("slot." s ".condition.resistorType"),     \
               indexOf("slot." s ".thermal.ambient"), indexOf("slot." s ".thermal.mass"),                    \
               indexOf("slot." s ".thermal.driftCoefficient"), indexOf("slot." s ".stage.drive"),            \
               indexOf("slot." s ".stage.activeElement"), indexOf("slot." s ".stage.bias"),                  \
               indexOf("slot." s ".transformer.core"), indexOf("slot." s ".noise.humFrequency"),             \
               indexOf("slot." s ".noise.humLevel"), indexOf("slot." s ".noise.thermalNoise"),               \
               indexOf("slot." s ".noise.microphonics"), indexOf("slot." s ".filter.lpfEnabled"),            \
               indexOf("slot." s ".filter.lpfFrequency")}
constexpr SlotParams kSlotP[kSlots] = {CD_SLOT("a"), CD_SLOT("b")};
#undef CD_SLOT
constexpr bool validSlot(const SlotParams& p) {
    const std::size_t v[] = {p.mains, p.railCap, p.rectifier, p.sag, p.age, p.tol, p.leak, p.resType, p.ambient, p.mass, p.drift,
                             p.drive, p.element, p.bias, p.core, p.humHz, p.humLevel, p.jn, p.micro, p.lpfOn, p.lpfHz};
    for (std::size_t x : v) if (x >= kParamCount) return false;
    return true;
}
static_assert(validSlot(kSlotP[0]) && validSlot(kSlotP[1]), "slot parameter IDs must exist in the registry");

constexpr int kSlice = 64;                       // internal slice (host samples)
constexpr int kFactorList[5] = {1, 2, 4, 8, 16};
constexpr int kMaxFactor = 16;
constexpr int kHistory = 512;                    // input history for oversampler warm-up
constexpr int kPerCh = 12;                       // ComponentId C_in..C_g2 are per channel
constexpr double kTref = 298.15;                 // tempco / noise reference temperature [K]
constexpr int kElemFam = kFamilyCount;

double dbToLin(double db) noexcept { return std::exp(db * (std::log(10.0) / 20.0)); }
int factorIndex(int f) noexcept {
    for (int i = 0; i < 5; ++i) if (kFactorList[i] == f) return i;
    return 2;
}
bool isPerChannel(int c) noexcept { return c < kPerCh; }

// SYNTHETIC thermal network constants per component (heat capacity J/K, conductance to chassis
// and to ambient W/K). Not measured; chosen for second-to-minute time constants (G01).
struct ThermalSpec { double cap, gChassis, gAmb; };
constexpr ThermalSpec kThermal[kComponentCount] = {
    {0.5, 0.01, 0.002}, {0.3, 0.01, 0.002}, {4.0, 0.03, 0.01}, {0.6, 0.012, 0.003}, {0.3, 0.01, 0.002},
    {2.0, 0.03, 0.005}, {2.0, 0.03, 0.005}, {60.0, 0.3, 0.1}, {3.0, 0.04, 0.005}, {0.6, 0.012, 0.003},
    {0.5, 0.012, 0.003}, {1.0, 0.02, 0.003}, {10.0, 0.08, 0.02}, {15.0, 0.1, 0.05}, {1.0, 0.02, 0.005},
    {5.0, 0.05, 0.01}, {400.0, 0.0, 2.0}};

// SYNTHETIC resistor technologies (slot.*.condition.resistorType): tempco [1/K], aging drift
// [1/year], excess-noise factor [dB] relative to Johnson noise.
struct ResistorTech { double tempco, driftPerYear, noiseDb; };
constexpr ResistorTech kResTech[3] = {{-8e-4, 2e-3, 10.0}, {5e-5, 2e-4, 0.0}, {2e-5, 1e-4, -1.0}};

// Hum shape: mains harmonics 1..5, normalized to unit RMS.
constexpr double kHumHarm[5] = {1.0, 0.5, 0.3, 0.15, 0.08};

struct OsBank {
    dsp::Oversampler up[kMaxChannels];
    dsp::Oversampler down[kSlots][kMaxChannels];
    DelayLine hiPad[kSlots][kMaxChannels], basePad[kSlots][kMaxChannels];
    dsp::Oversampler clipUp[kMaxChannels], clipDown[kMaxChannels];
    DelayLine clipHiPad[kMaxChannels], clipBasePad[kMaxChannels];
    int hiPadLen = 0, basePadLen = 0;
    double latency = 0;
    bool ok = false;
};

struct Lane {
    bool on = false;
    int fi = 2;
    CircuitPath path[kSlots][kFamilyCount][kMaxChannels];
    CircuitValues applied[kSlots][kFamilyCount][kMaxChannels];
};

struct FamilyNominal {
    double vFiltPu[3] = {1, 1, 1};   // idle filter-node rail per rectifier type at 117 V [p.u.]
    double vResPu[3] = {1, 1, 1};
    double iIdle = 0;                // idle supply current per channel [A]
    double outScale = 1;             // calibration: unity small-signal 1 kHz gain at default drive
    double powerIdle[kComponentCount] = {};
    bool ok = false;
};

}  // namespace

// ---------------------------------------------------------------------------------------------
struct CircuitEngine::Impl {
    // configuration
    EngineConfig cfg{};
    double fs = 48000;
    int nCh = 2;
    bool prepared = false, zeroLatency = false, iir = false, pitchDrift = false;
    int latC = 0, latCreative = 0, latTotal = 0;   // circuit/clip padded latency, creative base delay
    int warmup = 64, fade20 = 960, fade10 = 480, fade50 = 2400;
    FamilyNominal nominal[kFamilyCount];

    OsBank bank[5];
    Lane lanes[2];
    int laneA = 0;
    int qState = 0;          // 0 idle, 1 transition (warm-up then fade)
    long qPos = 0;
    int qTargetFi = 2;
    int activeFi = 2, newtonCap = 8;

    struct Slot {
        // smoothed parameters
        Smoother mains, railCap, sag, age, tol, leak, ambient, mass, drift, drive, bias, humHz, humLevel, jn, micro, lpfHz, shelf;
        Ramp lpfOn;
        int fam = 0, famNext = -1;
        long famPos = 0;
        int rect = 0, rectPrev = 0, core = 1, corePrev = 1, res = 1, resPrev = 1;
        double rectW = 1, coreW = 1, resW = 1;
        Supply supply;
        ThermalClock thermal;
        double temps[kMaxChannels][kComponentCount] = {};
        double tnom[kComponentCount] = {};
        double z[kMaxChannels][kComponentCount] = {}, u[kMaxChannels][kComponentCount] = {};
        ConditionSet cond{};
        double condValue[kComponentCount] = {}, condEsr[kComponentCount] = {}, condLeak[kComponentCount] = {},
               condNoise[kComponentCount] = {};
        // block targets (from params + modulation)
        double tMains = 117, tRailCapF = 1e-4, tSag = 0.5, tAge = 0, tTol = 2, tLeak = 0, tAmbient = 298.15, tMass = 0.5,
               tDrift = 1, tDrive = 6, tBias = 0, tHumHz = 50, tHumLevel = -120, tJn = -120, tMicro = 0, tLpfHz = 18000,
               tShelf = 0;
        bool tLpfOn = false;
        core::SlotModulation mod{};
        // values at the last thermal tick
        double ageNow = 0, sigmaNow = 0, leakNow = 0, tempcoNow = 1, fluxNow = 1;
        // noise / creative state
        Rng jnRng[kMaxChannels], ageRng[kMaxChannels];
        double micZ1 = 0, micZ2 = 0, micB1 = 0, micB2 = 0, micG = 1;
        Biquad lpf[kMaxChannels], shelfF[kMaxChannels];
        double lpfApplied = -1, shelfApplied = 0;
        DelayLine pitch[kMaxChannels];
        double pitchDelay = 0;   // samples
        // per-sample aggregation
        double power[kMaxChannels * kPerCh + 5] = {};
        double iStage = 0, iDyn = 0, vStage = 0;
        double railDc = 0, kRec = 1;
        EffectiveTargets targets{};
        // telemetry rings (channel 0)
        double railRing[kRingSize] = {}, bhRing[kRingSize][2] = {};
        double stageI[kMaxChannels] = {}, flux[kMaxChannels] = {}, hField[kMaxChannels] = {};
    } slot[kSlots];

    // global smoothers / ramps / filters
    Smoother inGain, outGain, mix, ab, hpfHz;
    Ramp hpfOn, clipOn, gmOn, bypass, fallback;
    double tInGain = 1, tOutGain = 1, tMix = 1, tAb = 0, tHpfHz = 15;
    bool tHpfOn = true, tClipOn = true, tGmOn = false, analogNoise = false, wantBypass = false;
    dsp::GainMatcher gm;
    Biquad hpf[kMaxChannels];
    double hpfApplied = -1;
    DelayLine dryDelay[kMaxChannels], rawDelay[kMaxChannels], clipDelay[kMaxChannels];
    DelayLine hist[kMaxChannels], preHist[kMaxChannels];
    double detFast = 0, detSlow = 0, detPrev = 0;
    double aFastAtt = 0, aFastRel = 0, aSlowAtt = 0, aSlowRel = 0;
    // fault state
    bool inFallback = false, needsReset = false, forceFail = false;
    int injectNaN = 0;
    long solverFailures = 0, nonFiniteOut = 0, clampedOut = 0, nonFiniteIn = 0;
    int consecutiveFail = 0;
    // clocks
    int64_t sampleIndex = 0;
    uint64_t epoch = 0;
    bool sawEpoch = false, rateMismatch = false;
    // telemetry
    TripleBuffer<TelemetryFrame> telemetry;
    TripleBuffer<ConditionSet> conditions[kSlots];
    double inPeak[kMaxChannels] = {}, inSq[kMaxChannels] = {}, outPeak[kMaxChannels] = {}, outSq[kMaxChannels] = {};
    long levelCount = 0;
    double xfer[kRingSize][2] = {};
    int ringPos = 0, xferPos = 0, ringDecim = 16, ringCounter = 0;
    uint64_t generation = 0;
    // warm / cold snapshots
    struct Snapshot {
        std::vector<double> path[kSlots][kFamilyCount][kMaxChannels];
        CircuitValues values[kSlots][kFamilyCount][kMaxChannels];
        double temps[kSlots][kMaxChannels][kComponentCount] = {};
        double supply[kSlots][3] = {};
        double bplus[kSlots][kFamilyCount][kMaxChannels] = {};
    } warm, cold;
    // scratch (slice buffers)
    double x[kMaxChannels][kSlice] = {}, dryAl[kMaxChannels][kSlice] = {}, raw[kMaxChannels][kSlice] = {};
    double hiIn[2][kMaxChannels][kSlice * kMaxFactor] = {};
    double hiOut[2][kSlots][kMaxChannels][kSlice * kMaxFactor] = {};
    double baseOut[2][kSlots][kMaxChannels][kSlice] = {};
    double pre[kMaxChannels][kSlice] = {}, clipOut[2][kMaxChannels][kSlice] = {};
    double hiTmp[kSlice * kMaxFactor] = {};
    double humPhase[kSlots][kSlice] = {}, excite[kSlice] = {}, extLoad[kSlice] = {}, preserveP[kSlice] = {};
    bool pendingIir = false, pendingZero = false, pendingPitch = false;
    std::vector<double> stateScratch;
    // Sized during prepare; a rejected nested restore can roll back without audio-thread allocation.
    Checkpoint checkpointBackup;

    // ---------------------------------------------------------------------------------------
    double laneWeight(long pos) const noexcept {
        if (pos < warmup) return 0.0;
        return std::min(1.0, double(pos - warmup + 1) / double(fade20));
    }
    double famWeight(long pos) const noexcept { return std::min(1.0, double(pos + 1) / double(fade20)); }
    static int factorFor(int mode, int over) noexcept {
        static constexpr int modeF[3] = {4, 4, 16};
        static constexpr int overF[6] = {0, 1, 2, 4, 8, 16};
        const int m = std::clamp(mode, 0, 2), o = std::clamp(over, 0, 5);
        return o == 0 ? modeF[m] : overF[o];
    }
    static int capFor(int mode) noexcept {
        static constexpr int caps[3] = {6, 8, 12};
        return caps[std::clamp(mode, 0, 2)];
    }
    static int en(double v, int lo, int hi) noexcept { return std::isfinite(v) ? std::clamp(int(std::lround(v)), lo, hi) : lo; }
    static double fin(double v, double lo, double hi, double def) noexcept { return std::isfinite(v) ? std::clamp(v, lo, hi) : def; }
    int thermalNode(int ch, int comp) const noexcept { return isPerChannel(comp) ? ch * kPerCh + comp : nCh * kPerCh + (comp - kPerCh); }
    int thermalNodes() const noexcept { return nCh * kPerCh + (kComponentCount - kPerCh); }

    // ---- component values -------------------------------------------------------------------
    CircuitValues valuesFor(int s, int ch, Family f) const noexcept;
    void applyValues(int s) noexcept;
    void refreshSlowState(int s) noexcept;
    // ---- processing -------------------------------------------------------------------------
    void beginBlock(const core::ParamValues& p, const core::ModulationBlock& mod, const ProcessContext& ctx) noexcept;
    void slotTargets(int s, const core::ParamValues& p, const core::SlotModulation& m) noexcept;
    void startQuality(int fi) noexcept;
    void startFamily(int s, int fam) noexcept;
    void slice(const double* const* in, double* const* out, int channels, int off, int n, const core::ModulationBlock& mod) noexcept;
    void circuitSample(int i, int channels, double extA, double preserve) noexcept;
    void publish(int channels) noexcept;
    void enterFallback() noexcept { inFallback = true; needsReset = true; }
    void doReset(ResetKind k) noexcept;
    // ---- preparation ------------------------------------------------------------------------
    bool prepareAll(const EngineConfig& c, const core::ParamValues& p, std::string& err);
    bool prepareThermal(int s, std::string& err);
    void steadyTemperatures(int s, const double* powerPerNode, double ambientK, double (*out)[kComponentCount]) const;
    // ---- checkpoint -------------------------------------------------------------------------
    template <class Io> void visit(Io& io);
    uint64_t layoutHash() const noexcept;
};

// ---- values ---------------------------------------------------------------------------------

CircuitValues CircuitEngine::Impl::valuesFor(int s, int ch, Family f) const noexcept {
    const Slot& S = slot[s];
    const FamilyDesign& d = familyDesign(f);
    const CoreMaterial& cm = coreMaterial(S.core), &cp = coreMaterial(S.corePrev);
    auto lg = [](double a, double b, double w) { return std::exp(std::log(a) + w * (std::log(b) - std::log(a))); };
    const ResistorTech& ra = kResTech[S.resPrev], &rb = kResTech[S.res];
    const double rTc = ra.tempco + S.resW * (rb.tempco - ra.tempco);
    const double rDrift = ra.driftPerYear + S.resW * (rb.driftPerYear - ra.driftPerYear);
    const double k = S.tempcoNow, age = S.ageNow, sig = S.sigmaNow;
    auto tol = [&](ComponentId c) { return 1.0 + std::clamp(sig * S.z[ch][int(c)], -0.25, 0.25); };
    auto dT = [&](ComponentId c) { return S.temps[ch][int(c)] - S.tnom[int(c)]; };
    auto cond = [&](ComponentId c) -> const ComponentCondition& { return S.cond.c[int(c)]; };
    auto res = [&](ComponentId c, double r0) {
        if (r0 <= 0) return 0.0;
        double r = r0 * tol(c) * (1.0 + rDrift * age * (1.0 + 0.5 * S.u[ch][int(c)])) * (1.0 + rTc * k * dT(c));
        r *= S.condValue[int(c)];
        if (cond(c).open) r *= 1000.0;
        if (cond(c).shortish) r *= 0.01;
        return std::clamp(r, 1e-3, 1e9);
    };
    auto cap = [&](ComponentId c, const CapValues& c0, double leakShort, bool elec, double extraEsr) {
        CapValues v;
        const double agingC = elec ? std::exp(-0.0085 * age) : std::exp(-0.001 * age);
        const double agingEsr = elec ? std::exp(0.035 * age) : 1.0;
        const double tcC = elec ? 2.5e-3 : -2e-4;
        v.c = c0.c * tol(c) * agingC * std::max(0.5, 1.0 + tcC * k * dT(c)) * S.condValue[int(c)];
        v.esr = c0.esr * agingEsr * std::exp(-0.015 * k * dT(c)) * S.condEsr[int(c)];
        double lf = std::clamp((elec ? S.leakNow : 0.25 * S.leakNow) + S.condLeak[int(c)], 0.0, 1.0);
        if (cond(c).shortish) lf = 1.0;
        v.leak = lg(c0.leak, leakShort, lf) * std::exp(-0.04 * k * dT(c));
        if (cond(c).open) { v.c *= 0.01; v.esr *= 1000.0; }
        v.esr += extraEsr;
        v.c = std::max(v.c, 1e-12);
        return v;
    };
    CircuitValues v;
    v.rG = res(ComponentId::R_g, d.rG);
    v.rTop = res(ComponentId::R_g, d.rTop);
    v.rLoad = res(ComponentId::R_load, d.rLoad);
    v.rK = res(ComponentId::R_k, d.rK);
    v.rDec = res(ComponentId::R_dec, d.rDec);
    v.rScreen = res(ComponentId::R_g2, d.rScreen);
    v.rRefl = d.rRefl;
    const bool elecIn = std::strcmp(d.cInModelId, "synthetic.electrolytic") == 0;
    v.cIn = cap(ComponentId::C_in, d.cIn, d.leakShort[0], elecIn, 0);
    v.cK = cap(ComponentId::C_k, d.cK, d.leakShort[1], true, d.kDegeneration);
    v.cOut = cap(ComponentId::C_out, d.cOut, d.leakShort[2], true, 0);
    v.cDec = cap(ComponentId::C_dec, d.cDec, d.leakShort[3], true, 0);
    v.cScreen = d.cScreen.c > 0 ? cap(ComponentId::C_g2, d.cScreen, d.leakShort[4], false, 0) : d.cScreen;
    // Transformer: core-material crossfade (log domain), tempco, flux drive, condition.
    const double dTt = dT(ComponentId::T_out);
    const double fluxF = std::clamp(S.fluxNow, 0.5, 2.0);
    v.lM = d.lM * lg(cp.inductanceScale, cm.inductanceScale, S.coreW) * (1.0 + 0.002 * k * dTt) * S.condValue[int(ComponentId::T_out)];
    v.lambdaS = d.lambdaS * lg(cp.fluxScale, cm.fluxScale, S.coreW) * std::max(0.5, 1.0 - 0.001 * k * dTt) / fluxF;
    v.exponent = cp.exponent + S.coreW * (cm.exponent - cp.exponent);
    v.rCore = d.rCore * lg(cp.lossScale, cm.lossScale, S.coreW);
    v.rWind = d.rWind * (1.0 + 0.0039 * k * dTt);
    if (cond(ComponentId::T_out).open) v.lM *= 0.01;
    // Active element: tolerance on transconductance, aging, temperature (deviation scaled by k).
    std::copy(d.dev, d.dev + 8, v.dev);
    const double tV = tol(ComponentId::V1), dTv = k * dT(ComponentId::V1), tEff = S.tnom[int(ComponentId::V1)] + dTv;
    const double vScale = S.condValue[int(ComponentId::V1)];
    switch (f) {
        case Family::Triode12AX7: case Family::PentodeEF86:
            v.dev[2] = d.dev[2] / tV * (1.0 + 0.01 * age) * std::max(0.2, 1.0 - 0.003 * dTv) / vScale;
            break;
        case Family::Bjt:
            v.dev[0] = d.dev[0] * tV * std::exp(0.069 * dTv) * vScale;
            v.dev[3] = d.dev[3] * tEff / S.tnom[int(ComponentId::V1)];
            break;
        case Family::Jfet:
            v.dev[0] = d.dev[0] * tV * std::max(0.2, 1.0 - 0.004 * dTv) * vScale;
            v.dev[1] = d.dev[1] + 0.002 * dTv;
            v.dev[4] = d.dev[4] * std::exp2(dTv / 10.0);
            v.dev[5] = d.dev[5] * tEff / S.tnom[int(ComponentId::V1)];
            break;
    }
    return v;
}

void CircuitEngine::Impl::applyValues(int s) noexcept {
    Slot& S = slot[s];
    for (int L = 0; L < 2; ++L) {
        if (!lanes[L].on) continue;
        for (int fam : {S.fam, S.famNext}) {
            if (fam < 0) continue;
            for (int ch = 0; ch < nCh; ++ch) {
                CircuitValues v = valuesFor(s, ch, Family(fam));
                lanes[L].applied[s][fam][ch] = v;
                lanes[L].path[s][fam][ch].apply(v);
            }
        }
    }
}

// Slow per-slot state sampled at the thermal tick (1 kHz): aging, tolerance, leakage, tempco,
// flux drive and condition overlays (log-domain 10 ms smoothing), thermal mass and ambient.
void CircuitEngine::Impl::refreshSlowState(int s) noexcept {
    Slot& S = slot[s];
    const core::SlotModulation& m = S.mod;
    S.ageNow = std::clamp(std::max(S.age.y, fin(m.ageYearsMin, 0, 100, 0)), 0.0, 100.0);
    S.sigmaNow = std::clamp(S.tol.y / 300.0 + fin(m.toleranceSigmaAdd, 0, 1, 0), 0.0, 0.25 / 3.0);
    S.leakNow = std::clamp(S.leak.y + fin(m.leakageAdd, 0, 1, 0) + 0.004 * S.ageNow, 0.0, 1.0);
    S.tempcoNow = std::clamp(S.drift.y * fin(m.thermalSensitivityScale, 0, 10, 1), 0.0, 20.0);
    S.fluxNow = fin(m.fluxDriveFactor, 0.5, 2.0, 1.0);
    const double a = std::exp(-1.0 / (ThermalClock::kRateHz * 0.010));
    for (int c = 0; c < kComponentCount; ++c) {
        const ComponentCondition& cc = S.cond.c[c];
        auto sm = [&](double& st, double target) { st = std::exp(std::log(st) + (1.0 - a) * (std::log(target) - std::log(st))); };
        sm(S.condValue[c], fin(cc.valueScale, 0.05, 20.0, 1.0));
        sm(S.condEsr[c], fin(cc.esrScale, 1.0, 1000.0, 1.0));
        sm(S.condNoise[c], std::max(1e-6, fin(cc.noiseScale, 0.0, 100.0, 1.0)));
        S.condLeak[c] += (1.0 - a) * (fin(cc.leakage, 0.0, 1.0, 0.0) - S.condLeak[c]);
    }
    S.thermal.network().setAmbient(S.ambient.y);
    S.thermal.network().setCapacityScale(0.25 * std::pow(16.0, std::clamp(S.mass.y, 0.0, 1.0)));
}

// ---- preparation ------------------------------------------------------------------------------

namespace {
// Static per-family nominal analysis (deterministic, computed once, thread-safe init): idle supply
// rails per rectifier at 117 V mains, idle stage current, output calibration (unity 1 kHz
// small-signal gain at the registry-default drive, 192 kHz reference rate) and idle powers.
const FamilyNominal& familyNominal(Family f) {
    static const std::array<FamilyNominal, kFamilyCount> table = [] {
        std::array<FamilyNominal, kFamilyCount> t{};
        constexpr double fsCal = 192000.0;
        const double defaultDrive = core::kParams[kSlotP[0].drive].def;
        for (int fi = 0; fi < kFamilyCount; ++fi) {
            FamilyNominal& n = t[fi];
            const FamilyDesign& d = familyDesign(Family(fi));
            const CircuitValues v = nominalValues(Family(fi), 1);
            std::string err;
            CircuitPath p;
            if (!p.build(Family(fi), v, 1.0 / fsCal, 12, d.railDesignV * 0.95, err)) continue;
            // Iterate supply idle <-> stage idle current twice.
            double iIdle = p.supplyCurrent();
            for (int pass = 0; pass < 2; ++pass) {
                for (int r = 0; r < 3; ++r) {
                    Supply sup;
                    sup.prepare(48000.0);
                    sup.setState(1.0, 1.0, 0.0);
                    const double iStagePu = 2.0 * iIdle / d.designA, aux = std::max(0.0, 1.0 - iStagePu);
                    double accF = 0, accR = 0;
                    const int N = 48000 * 3 / 2, avg = 48000 / 2;  // settle >= 7 reservoir time constants
                    for (int k = 0; k < N; ++k) {
                        sup.step(117.0, 50.0, iStagePu, aux, 1e-4 * d.railDesignV / d.designA, 160.0, rectifierLaw(r), 1.0);
                        if (k >= N - avg) { accF += sup.filter(); accR += sup.reservoir(); }
                    }
                    n.vFiltPu[r] = accF / avg;
                    n.vResPu[r] = accR / avg;
                }
                CircuitPath q;
                if (!q.build(Family(fi), v, 1.0 / fsCal, 12, n.vFiltPu[0] * d.railDesignV, err)) break;
                iIdle = q.supplyCurrent();
                p = q;
            }
            n.iIdle = iIdle;
            // Calibration: 1 kHz sine, small level, default drive; quadrature over 10 periods.
            const double g = dbToLin(defaultDrive) * d.inVoltsPerFs, amp = 1e-3;
            double sc = 0, cc = 0;
            const int per = int(fsCal / 1000.0), N = per * 40;
            CircuitPath c = p;
            for (int k = 0; k < N; ++k) {
                const double w = 2.0 * kPi * 1000.0 * k / fsCal;
                c.setDrive(sourceCompliance(amp * g * std::sin(w), complianceVolts(d)));
                c.step();
                if (k >= N - per * 10) { sc += c.outputVolts() * std::sin(w); cc += c.outputVolts() * std::cos(w); }
            }
            // Unity |gain| at 1 kHz; the sign of the in-phase component makes the slot non-inverting.
            const double inPhase = 2.0 * sc / (per * 10) / amp, quad = 2.0 * cc / (per * 10) / amp;
            const double mag = std::hypot(inPhase, quad);
            n.outScale = mag > 1e-12 ? std::copysign(1.0 / mag, inPhase) : 1.0;
            for (double& x : n.powerIdle) x = 0;
            p.accumulatePower(n.powerIdle);
            n.ok = true;
        }
        return t;
    }();
    return table[std::clamp(int(f), 0, kFamilyCount - 1)];
}
}  // namespace

void CircuitEngine::Impl::steadyTemperatures(int s, const double* pNode, double ambientK, double (*out)[kComponentCount]) const {
    // Direct steady state G T = P + G_amb T_amb (dense Gauss, off-thread only).
    (void)s;
    const int n = thermalNodes();
    std::vector<double> A(std::size_t(n) * n, 0.0), b(n, 0.0);
    auto add = [&](int i, int j, double g) { A[std::size_t(i) * n + j] += g; };
    for (int ch = 0; ch < nCh; ++ch)
        for (int c = 0; c < kPerCh; ++c) {
            const int i = thermalNode(ch, c), k = thermalNode(0, int(ComponentId::Chassis));
            add(i, i, kThermal[c].gAmb + kThermal[c].gChassis); add(k, k, kThermal[c].gChassis);
            add(i, k, -kThermal[c].gChassis); add(k, i, -kThermal[c].gChassis);
            b[i] += kThermal[c].gAmb * ambientK;
        }
    for (int c = kPerCh; c < kComponentCount; ++c) {
        const int i = thermalNode(0, c), k = thermalNode(0, int(ComponentId::Chassis));
        add(i, i, kThermal[c].gAmb);
        b[i] += kThermal[c].gAmb * ambientK;
        if (c != int(ComponentId::Chassis)) {
            add(i, i, kThermal[c].gChassis); add(k, k, kThermal[c].gChassis);
            add(i, k, -kThermal[c].gChassis); add(k, i, -kThermal[c].gChassis);
        }
    }
    for (int i = 0; i < n; ++i) b[i] += pNode[i];
    for (int k = 0; k < n; ++k) {
        int p = k;
        for (int r = k + 1; r < n; ++r) if (std::fabs(A[std::size_t(r) * n + k]) > std::fabs(A[std::size_t(p) * n + k])) p = r;
        for (int c = 0; c < n; ++c) std::swap(A[std::size_t(k) * n + c], A[std::size_t(p) * n + c]);
        std::swap(b[k], b[p]);
        for (int r = k + 1; r < n; ++r) {
            const double f = A[std::size_t(r) * n + k] / A[std::size_t(k) * n + k];
            for (int c = k; c < n; ++c) A[std::size_t(r) * n + c] -= f * A[std::size_t(k) * n + c];
            b[r] -= f * b[k];
        }
    }
    std::vector<double> T(n);
    for (int k = n - 1; k >= 0; --k) {
        double sum = b[k];
        for (int c = k + 1; c < n; ++c) sum -= A[std::size_t(k) * n + c] * T[c];
        T[k] = sum / A[std::size_t(k) * n + k];
    }
    for (int ch = 0; ch < kMaxChannels; ++ch)
        for (int c = 0; c < kComponentCount; ++c) out[ch][c] = T[thermalNode(std::min(ch, nCh - 1), c)];
}

bool CircuitEngine::Impl::prepareThermal(int s, std::string& err) {
    dsp::ThermalNetwork::Config tc;
    tc.n = std::size_t(thermalNodes());
    const int chassis = thermalNode(0, int(ComponentId::Chassis));
    for (int ch = 0; ch < nCh; ++ch)
        for (int c = 0; c < kPerCh; ++c) {
            const int i = thermalNode(ch, c);
            tc.capacity[i] = kThermal[c].cap;
            tc.toAmbient[i] = kThermal[c].gAmb;
            tc.coupling[i][chassis] = tc.coupling[chassis][i] = kThermal[c].gChassis;
        }
    for (int c = kPerCh; c < kComponentCount; ++c) {
        const int i = thermalNode(0, c);
        tc.capacity[i] = kThermal[c].cap;
        tc.toAmbient[i] = kThermal[c].gAmb;
        if (c != int(ComponentId::Chassis)) tc.coupling[i][chassis] = tc.coupling[chassis][i] = kThermal[c].gChassis;
    }
    if (!slot[s].thermal.prepare(tc, fs, slot[s].tAmbient)) { err = "thermal network preparation failed"; return false; }
    return true;
}

bool CircuitEngine::Impl::prepareAll(const EngineConfig& config, const core::ParamValues& p, std::string& err) {
    if (!(config.sampleRate >= 8000 && config.sampleRate <= 768000) || !std::isfinite(config.sampleRate)) { err = "sample rate outside 8 kHz..768 kHz"; return false; }
    if (config.maxBlock < 1 || config.maxBlock > 1 << 20) { err = "maxBlock outside 1..1048576"; return false; }
    if (config.channels < 1 || config.channels > kMaxChannels) { err = "channels must be 1 or 2"; return false; }
    cfg = config;
    fs = config.sampleRate;
    nCh = config.channels;
    zeroLatency = en(p[kPZeroLatency], 0, 1) != 0;
    iir = en(p[kPPhase], 0, 1) != 0;
    pitchDrift = en(p[kPPitchDrift], 0, 1) != 0;
    fade20 = std::max(1, int(std::lround(0.020 * fs)));
    fade10 = std::max(1, int(std::lround(0.010 * fs)));
    fade50 = std::max(1, int(std::lround(0.050 * fs)));
    ringDecim = std::max(1, int(std::lround(fs / 3000.0)));
    for (int f = 0; f < kFamilyCount; ++f) {
        nominal[f] = familyNominal(Family(f));
        if (!nominal[f].ok) { err = "family nominal analysis failed (synthetic circuit DC)"; return false; }
    }
    // ---- oversampling banks and latency (padded within the phase family over 1..16x) ----
    const dsp::OsFamily osFam = iir ? dsp::OsFamily::Iir : dsp::OsFamily::Fir;
    double maxLat = 0;
    for (int i = 0; i < 5; ++i) {
        OsBank& b = bank[i];
        b.ok = false;
        if (zeroLatency && kFactorList[i] != 1) continue;
        const dsp::Oversampler::Spec spec{kFactorList[i], osFam, 0.45, 100.0};
        bool ok = true;
        for (int ch = 0; ch < kMaxChannels; ++ch) {
            ok = ok && b.up[ch].prepare(spec, kSlice) && b.clipUp[ch].prepare(spec, kSlice) && b.clipDown[ch].prepare(spec, kSlice);
            for (int s = 0; s < kSlots; ++s) ok = ok && b.down[s][ch].prepare(spec, kSlice);
        }
        if (!ok) { err = "oversampler preparation failed"; return false; }
        b.latency = b.up[0].latency();
        maxLat = std::max(maxLat, b.latency);
        b.ok = true;
    }
    latC = zeroLatency ? 0 : int(std::ceil(maxLat - 1e-9));
    for (int i = 0; i < 5; ++i) {
        OsBank& b = bank[i];
        if (!b.ok) continue;
        const int F = kFactorList[i];
        const double D = zeroLatency ? 0.0 : latC - b.latency;
        int base = int(std::floor(D + 1e-9));
        int hi = int(std::lround((D - base) * F));
        if (hi >= F) { base += 1; hi -= F; }
        b.basePadLen = std::max(0, base);
        b.hiPadLen = std::max(0, hi);
        for (int ch = 0; ch < kMaxChannels; ++ch) {
            for (int s = 0; s < kSlots; ++s) {
                b.hiPad[s][ch].allocate(b.hiPadLen + 1); b.hiPad[s][ch].setDelay(b.hiPadLen);
                b.basePad[s][ch].allocate(b.basePadLen + 1); b.basePad[s][ch].setDelay(b.basePadLen);
            }
            b.clipHiPad[ch].allocate(b.hiPadLen + 1); b.clipHiPad[ch].setDelay(b.hiPadLen);
            b.clipBasePad[ch].allocate(b.basePadLen + 1); b.clipBasePad[ch].setDelay(b.basePadLen);
        }
    }
    latCreative = (pitchDrift && !zeroLatency) ? int(std::lround(1.5e-3 * fs)) : 0;
    latTotal = 2 * latC + latCreative;
    warmup = 2 * latC + 64;
    for (int ch = 0; ch < kMaxChannels; ++ch) {
        dryDelay[ch].allocate(latC + latCreative + 1); dryDelay[ch].setDelay(latC + latCreative);
        rawDelay[ch].allocate(latTotal + 1); rawDelay[ch].setDelay(latTotal);
        clipDelay[ch].allocate(latC + 1); clipDelay[ch].setDelay(latC);
        hist[ch].allocate(kHistory); preHist[ch].allocate(kHistory);
    }
    // ---- global parameters ----
    const int mode = en(p[kPQualityMode], 0, 2), over = en(p[kPOversampling], 0, 5);
    activeFi = zeroLatency ? 0 : factorIndex(factorFor(mode, over));
    qTargetFi = activeFi;
    newtonCap = capFor(mode);
    for (auto* r : {&hpfOn, &clipOn, &gmOn, &bypass, &fallback}) r->w = 0;
    hpfOn.setLength(fade20); clipOn.setLength(fade20); gmOn.setLength(fade50); bypass.setLength(fade10); fallback.setLength(fade10);
    inGain.setTime(fs, 0.010); outGain.setTime(fs, 0.010); mix.setTime(fs, 0.010); ab.setTime(fs, 0.020); hpfHz.setTime(fs, 0.020);
    gm.prepare(fs);
    const double t1 = 1.0 / (fs * 0.0002), t2 = 1.0 / (fs * 0.010), t3 = 1.0 / (fs * 0.020), t4 = 1.0 / (fs * 0.100);
    aFastAtt = std::exp(-t1); aFastRel = std::exp(-t2); aSlowAtt = std::exp(-t3); aSlowRel = std::exp(-t4);
    // ---- slots ----
    for (int s = 0; s < kSlots; ++s) {
        Slot& S = slot[s];
        for (auto* sm : {&S.mains, &S.railCap, &S.age, &S.tol, &S.leak, &S.ambient, &S.mass, &S.drift}) sm->setTime(fs, 0.050);
        for (auto* sm : {&S.sag, &S.drive, &S.bias, &S.humHz, &S.humLevel, &S.jn, &S.micro, &S.lpfHz, &S.shelf}) sm->setTime(fs, 0.020);
        S.lpfOn.setLength(fade20);
        core::ModulationBlock none{};
        slotTargets(s, p, none.slot[s]);
        S.fam = int(S.targets.family);
        S.rect = S.rectPrev = S.targets.rectifier;
        S.core = S.corePrev = S.targets.core;
        S.res = S.resPrev = S.targets.resistorType;
        // snap smoothers to targets (no ramps at start)
        S.mains.snap(S.tMains); S.railCap.snap(S.tRailCapF); S.sag.snap(S.tSag); S.age.snap(S.tAge); S.tol.snap(S.tTol);
        S.leak.snap(S.tLeak); S.ambient.snap(S.tAmbient); S.mass.snap(S.tMass); S.drift.snap(S.tDrift); S.drive.snap(S.tDrive);
        S.bias.snap(S.tBias); S.humHz.snap(S.tHumHz); S.humLevel.snap(S.tHumLevel); S.jn.snap(S.tJn); S.micro.snap(S.tMicro);
        S.lpfHz.snap(S.tLpfHz); S.shelf.snap(S.tShelf); S.lpfOn.w = S.tLpfOn ? 1.0 : 0.0;
        for (int c = 0; c < kComponentCount; ++c) { S.condValue[c] = S.condEsr[c] = S.condNoise[c] = 1.0; S.condLeak[c] = 0.0; }
        // tolerance and aging draws (variation seed: slot-stable, L/R and component specific)
        for (int ch = 0; ch < kMaxChannels; ++ch)
            for (int c = 0; c < kComponentCount; ++c) {
                Rng r;
                r.seed(cfg.variationSeed[s], 0x7A, uint64_t(ch * 64 + c), 1);
                S.z[ch][c] = std::clamp(r.gaussian(), -3.0, 3.0);
                S.u[ch][c] = r.uniform();
            }
        for (int ch = 0; ch < kMaxChannels; ++ch) {
            S.jnRng[ch].seed(cfg.seed, uint64_t(s), uint64_t(ch), 2);
            S.ageRng[ch].seed(cfg.seed, uint64_t(s), uint64_t(ch), 3);
            S.pitch[ch].allocate(int(std::ceil(0.008 * fs)) + 8);
        }
        S.pitchDelay = latCreative;
        S.supply.prepare(fs);
        if (!prepareThermal(s, err)) return false;
        // microphonic mode: f = 800 Hz, Q = 10; g normalizes the peak impulse response to 1.
        {
            const double r = std::exp(-kPi * 800.0 / (10.0 * fs)), w = 2 * kPi * 800.0 / fs;
            S.micB1 = 2 * r * std::cos(w); S.micB2 = -r * r;
            double z1 = 0, z2 = 0, peak = 0;
            for (int k = 0; k < int(fs * 0.01); ++k) {
                const double zz = S.micB1 * z1 + S.micB2 * z2 + (k == 0 ? 1.0 : 0.0);
                z2 = z1; z1 = zz; peak = std::max(peak, std::fabs(zz));
            }
            S.micG = peak > 0 ? 1.0 / peak : 1.0;
        }
    }
    // ---- build circuits (both lanes; lane 0 active) ----
    for (int L = 0; L < 2; ++L) { lanes[L].on = false; lanes[L].fi = activeFi; }
    lanes[0].on = true;
    laneA = 0; qState = 0; qPos = 0;
    const double h = 1.0 / (fs * kFactorList[activeFi]);
    for (int s = 0; s < kSlots; ++s) {
        Slot& S = slot[s];
        // Supply idle state for this slot's rectifier at its mains/rail settings.
        const FamilyDesign& d = familyDesign(Family(S.fam));
        const double iStagePu = nCh * nominal[S.fam].iIdle / d.designA, aux = std::max(0.0, 1.0 - iStagePu);
        S.supply.setState(nominal[S.fam].vResPu[S.rect], nominal[S.fam].vFiltPu[S.rect], 0.0);
        for (int k = 0; k < int(fs); ++k)  // 1 s from the nominal settled state
            S.supply.step(S.tMains, S.tHumHz, iStagePu, aux, std::max(4.7e-6, S.tRailCapF) * d.railDesignV / d.designA, 160.0,
                          rectifierLaw(S.rect), 1.0);
        // keep the settled mains phase (consistent rectifier conduction state)
        // Nominal temperatures (reference for tempco) and warm idle equilibrium at this ambient.
        std::vector<double> pn(std::size_t(thermalNodes()), 0.0);
        auto fillPower = [&](const double* perChannel, double heater, double rectHeater, double rectLoss) {
            std::fill(pn.begin(), pn.end(), 0.0);
            for (int ch = 0; ch < nCh; ++ch)
                for (int cc = 0; cc < kPerCh; ++cc) pn[thermalNode(ch, cc)] = perChannel[cc] + (cc == int(ComponentId::V1) ? heater : 0.0);
            pn[thermalNode(0, int(ComponentId::D_rect))] = rectHeater + rectLoss;
        };
        double tn[kMaxChannels][kComponentCount];
        fillPower(nominal[S.fam].powerIdle, d.heaterW, rectifierLaw(0).heaterW, 0.0);
        steadyTemperatures(s, pn.data(), kTref, tn);
        for (int cc = 0; cc < kComponentCount; ++cc) S.tnom[cc] = tn[0][cc];
        // Initial values at nominal temperature, then warm equilibrium at the slot ambient.
        for (int ch = 0; ch < kMaxChannels; ++ch)
            for (int cc = 0; cc < kComponentCount; ++cc) S.temps[ch][cc] = S.tnom[cc];
        refreshSlowState(s);
        for (int c2 = 0; c2 < kComponentCount; ++c2) { S.condValue[c2] = S.condEsr[c2] = S.condNoise[c2] = 1.0; S.condLeak[c2] = 0.0; }
        fillPower(nominal[S.fam].powerIdle, d.heaterW, rectifierLaw(S.rect).heaterW, 0.0);
        double tw[kMaxChannels][kComponentCount];
        steadyTemperatures(s, pn.data(), S.tAmbient, tw);
        double ta[kMaxChannels][kComponentCount];
        for (int ch = 0; ch < kMaxChannels; ++ch)
            for (int cc = 0; cc < kComponentCount; ++cc) ta[ch][cc] = S.tAmbient;
        // Build every family path (warm and cold DC snapshots).
        for (int pass = 0; pass < 2; ++pass) {
            auto& snap = pass == 0 ? warm : cold;
            const auto& T = pass == 0 ? tw : ta;
            for (int ch = 0; ch < kMaxChannels; ++ch)
                for (int cc = 0; cc < kComponentCount; ++cc) S.temps[ch][cc] = T[ch][cc];
            for (int fam = 0; fam < kFamilyCount; ++fam) {
                const FamilyDesign& fd = familyDesign(Family(fam));
                const double vRail = S.supply.filter() * fd.railDesignV;
                for (int ch = 0; ch < nCh; ++ch) {
                    const CircuitValues v = valuesFor(s, ch, Family(fam));
                    CircuitPath& path = lanes[0].path[s][fam][ch];
                    if (!path.build(Family(fam), v, h, newtonCap, vRail, err)) return false;
                    lanes[0].applied[s][fam][ch] = v;
                    snap.values[s][fam][ch] = v;
                    snap.path[s][fam][ch].resize(std::size_t(path.solver().stateSize()));
                    path.solver().getState(snap.path[s][fam][ch].data());
                    snap.bplus[s][fam][ch] = path.localBplus();
                }
            }
            for (int ch = 0; ch < kMaxChannels; ++ch)
                for (int cc = 0; cc < kComponentCount; ++cc) snap.temps[s][ch][cc] = T[ch][cc];
            snap.supply[s][0] = S.supply.reservoir(); snap.supply[s][1] = S.supply.filter(); snap.supply[s][2] = S.supply.phase();
        }
        // Warm start: rebuild lane paths from the warm snapshot.
        for (int fam = 0; fam < kFamilyCount; ++fam)
            for (int ch = 0; ch < nCh; ++ch) {
                CircuitPath& path = lanes[0].path[s][fam][ch];
                path.apply(warm.values[s][fam][ch]);
                lanes[0].applied[s][fam][ch] = warm.values[s][fam][ch];
                path.solver().setState(warm.path[s][fam][ch].data());
                path.bplusRef() = warm.bplus[s][fam][ch];
                path.refreshLoads();
                lanes[1].path[s][fam][ch] = path;  // second lane storage (allocated here, off-thread)
                lanes[1].applied[s][fam][ch] = warm.values[s][fam][ch];
            }
        for (int ch = 0; ch < kMaxChannels; ++ch)
            for (int cc = 0; cc < kComponentCount; ++cc) S.temps[ch][cc] = tw[ch][cc];
        std::array<double, dsp::ThermalNetwork::kMaxNodes> tnode{};
        for (int ch = 0; ch < nCh; ++ch)
            for (int cc = 0; cc < kPerCh; ++cc) tnode[thermalNode(ch, cc)] = tw[ch][cc];
        for (int cc = kPerCh; cc < kComponentCount; ++cc) tnode[thermalNode(0, cc)] = tw[0][cc];
        S.thermal.network().setState(tnode.data());
        S.railDc = S.supply.filter() * d.railDesignV;
        S.lpfApplied = -1; S.shelfApplied = 1e9;
    }
    stateScratch.assign(8192, 0.0);
    hpfApplied = -1;
    tInGain = dbToLin(fin(p[kPInGain], -24, 24, 0)); inGain.snap(tInGain);
    tOutGain = dbToLin(fin(p[kPOutGain], -24, 24, 0)); outGain.snap(tOutGain);
    tMix = fin(p[kPMix], 0, 1, 1); mix.snap(tMix);
    tAb = fin(p[kPAb], 0, 1, 0); ab.snap(tAb);
    tHpfHz = fin(p[kPHpfHz], 10, 250, 15); hpfHz.snap(tHpfHz);
    tHpfOn = en(p[kPHpfOn], 0, 1) != 0; hpfOn.w = tHpfOn ? 1 : 0;
    tClipOn = en(p[kPSoftClip], 0, 1) != 0; clipOn.w = tClipOn ? 1 : 0;
    tGmOn = en(p[kPGainMatch], 0, 1) != 0; gmOn.w = tGmOn ? 1 : 0;
    inFallback = needsReset = forceFail = false;
    injectNaN = 0;
    solverFailures = nonFiniteOut = clampedOut = nonFiniteIn = 0;
    consecutiveFail = 0;
    sampleIndex = 0; sawEpoch = false; rateMismatch = false;
    generation = 0;
    prepared = true;
    doReset(ResetKind::Warm);
    return true;
}


// ---- block targets ----------------------------------------------------------------------------

namespace {
// Synthetic transformer geometry per family (B = lambda / (N*A), H = N*i / l_e): N*A [m^2 turns],
// N/l_e [turns/m]. Chosen so Steel saturation onset reads ~1.25 T. Not measured cores.
double turnsArea(Family f) noexcept { return (f == Family::Bjt || f == Family::Jfet) ? 0.032 : 0.32; }
double turnsPerLength(Family f) noexcept { return (f == Family::Bjt || f == Family::Jfet) ? 8000.0 : 20000.0; }
bool isTube(int fam) noexcept { return fam == int(Family::Triode12AX7) || fam == int(Family::PentodeEF86); }
}  // namespace

void CircuitEngine::Impl::slotTargets(int s, const core::ParamValues& p, const core::SlotModulation& m) noexcept {
    Slot& S = slot[s];
    const SlotParams& q = kSlotP[s];
    S.mod = m;
    S.tMains = fin(p[q.mains], 90, 130, 117);
    const double capBase = fin(p[q.railCap], 10, 1000, 100) * 1e-6;
    const double div = fin(m.reservoirCapDivisor, 1, 100, 1);
    S.tRailCapF = std::max(4.7e-6, capBase / div);
    S.tSag = fin(p[q.sag], 0, 1, 0.5);
    S.tAge = fin(p[q.age], 0, 60, 0);
    S.tTol = fin(p[q.tol], 0, 25, 2);
    S.tLeak = fin(p[q.leak], 0, 1, 0);
    S.tAmbient = fin(p[q.ambient], 10, 50, 25) + 273.15;
    S.tMass = fin(p[q.mass], 0, 1, 0.5);
    S.tDrift = fin(p[q.drift], 0, 2, 1);
    S.tDrive = fin(p[q.drive], 0, 36, 6);
    S.tBias = fin(p[q.bias], -1, 1, 0);
    S.tHumHz = en(p[q.humHz], 0, 1) ? 60.0 : 50.0;
    S.tHumLevel = fin(p[q.humLevel], -120, -40, -120);
    S.tJn = fin(p[q.jn], -120, -50, -120);
    S.tMicro = fin(p[q.micro], 0, 1, 0);
    S.tLpfOn = en(p[q.lpfOn], 0, 1) != 0;
    S.tLpfHz = core::cutoffBaseHz(p[q.lpfHz]);
    S.tShelf = fin(m.hfShelfDb, -24, 24, 0);
    EffectiveTargets& t = S.targets;
    t.family = en(p[q.element], 0, 3);
    t.rectifier = en(p[q.rectifier], 0, 2);
    t.core = en(p[q.core], 0, 2);
    t.resistorType = en(p[q.resType], 0, 2);
    const FamilyDesign& d = familyDesign(Family(t.family));
    t.driveDb = S.tDrive + fin(m.driveOffsetDb, -24, 36, 0);
    t.biasNormalized = std::clamp(S.tBias + fin(m.biasOffset, -1, 1, 0), -1.0, 1.0);
    t.biasVolts = t.biasNormalized * d.biasSpanV;
    t.ageYears = std::clamp(std::max(S.tAge, fin(m.ageYearsMin, 0, 100, 0)), 0.0, 100.0);
    t.toleranceSigma = std::clamp(S.tTol / 300.0 + fin(m.toleranceSigmaAdd, 0, 1, 0), 0.0, 0.25 / 3.0);
    t.leakageFraction = std::clamp(S.tLeak + fin(m.leakageAdd, 0, 1, 0) + 0.004 * t.ageYears, 0.0, 1.0);
    t.reservoirCapBaseF = capBase;
    t.reservoirCapF = S.tRailCapF;
    t.sidechainMode = std::clamp(m.sidechainMode, 0, 2);
    t.sagAugmentation = t.sidechainMode == 1 ? 0.0 : 3.0 * fin(m.sagMacro, 0, 1, 0);
    t.fluxDriveFactor = fin(m.fluxDriveFactor, 0.5, 2.0, 1.0);
    t.saturationFluxWb = d.lambdaS * coreMaterial(t.core).fluxScale / t.fluxDriveFactor;
    t.tempcoScale = std::clamp(S.tDrift * fin(m.thermalSensitivityScale, 0, 10, 1), 0.0, 20.0);
    t.hfShelfDb = S.tShelf;
    t.creativeCutoffOct = fin(m.creativeCutoffOct, -core::kCreativeCutoffOctaves, core::kCreativeCutoffOctaves, 0);
    t.creativeDelayMs = fin(m.creativeDelayMs, -3, 3, 0);
    t.creativeDelayActive = m.creativeDelayActive && pitchDrift && !zeroLatency;
    t.recoveryTauS = fin(m.recoveryTauS, 0, 60, 0);
    const double tRes = S.tRailCapF * d.railDesignV / d.designA;
    t.recoveryConductanceScale =
        t.recoveryTauS > 0 ? std::clamp(Supply::naturalTau(tRes, rectifierLaw(t.rectifier)) / t.recoveryTauS, 0.05, 20.0) : 1.0;
    t.ageNoiseDbfs = fin(m.ageNoiseDbfs, -200, 0, -200);
    t.ageNoiseGated = !analogNoise;
    t.thermalMassScale = 0.25 * std::pow(16.0, S.tMass);
    t.ambientK = S.tAmbient;
    t.mainsVolts = S.tMains;
    t.mainsHz = S.tHumHz;
}

void CircuitEngine::Impl::beginBlock(const core::ParamValues& p, const core::ModulationBlock& mod, const ProcessContext& ctx) noexcept {
    for (int s = 0; s < kSlots; ++s)
        if (conditions[s].acquire()) slot[s].cond = conditions[s].front();
    if (sawEpoch && ctx.epoch != epoch)
        for (auto& S : slot) S.pitchDelay = latCreative;  // seek: creative pitch state re-centres
    epoch = ctx.epoch;
    sawEpoch = true;
    rateMismatch = ctx.sampleRate != fs;
    tInGain = dbToLin(fin(p[kPInGain], -24, 24, 0));
    tOutGain = dbToLin(fin(p[kPOutGain], -24, 24, 0));
    tMix = fin(p[kPMix], 0, 1, 1);
    tAb = fin(p[kPAb], 0, 1, 0);
    tHpfHz = std::min(fin(p[kPHpfHz], 10, 250, 15), 0.45 * fs);
    tHpfOn = en(p[kPHpfOn], 0, 1) != 0;
    tClipOn = en(p[kPSoftClip], 0, 1) != 0;
    tGmOn = en(p[kPGainMatch], 0, 1) != 0;
    analogNoise = en(p[kPAnalogNoise], 0, 1) != 0;
    pendingIir = en(p[kPPhase], 0, 1) != 0;
    pendingZero = en(p[kPZeroLatency], 0, 1) != 0;
    pendingPitch = en(p[kPPitchDrift], 0, 1) != 0;
    const int mode = en(p[kPQualityMode], 0, 2), over = en(p[kPOversampling], 0, 5);
    qTargetFi = zeroLatency ? 0 : factorIndex(factorFor(mode, over));
    const int cap = capFor(mode);
    if (cap != newtonCap) {
        newtonCap = cap;
        for (auto& L : lanes)
            for (auto& sp : L.path)
                for (auto& fp : sp)
                    for (auto& cp : fp) cp.solver().setMaxNewton(cap);
    }
    bool famBusy = false;
    for (int s = 0; s < kSlots; ++s) {
        slotTargets(s, p, mod.slot[s]);
        Slot& S = slot[s];
        const EffectiveTargets& t = S.targets;
        if (t.rectifier != S.rect && S.rectW >= 1.0) { S.rectPrev = S.rect; S.rect = t.rectifier; S.rectW = 0; }
        if (t.core != S.core && S.coreW >= 1.0) { S.corePrev = S.core; S.core = t.core; S.coreW = 0; }
        if (t.resistorType != S.res && S.resW >= 1.0) { S.resPrev = S.res; S.res = t.resistorType; S.resW = 0; }
        if (t.family != S.fam && S.famNext < 0 && qState == 0) startFamily(s, t.family);
        famBusy = famBusy || S.famNext >= 0;
    }
    if (qState == 0 && !famBusy && qTargetFi != activeFi) startQuality(qTargetFi);
}

void CircuitEngine::Impl::startFamily(int s, int fam) noexcept {
    Slot& S = slot[s];
    S.famNext = fam;
    S.famPos = 0;
    for (int L = 0; L < 2; ++L) {
        if (!lanes[L].on) continue;
        const double h = 1.0 / (fs * kFactorList[lanes[L].fi]);
        for (int ch = 0; ch < nCh; ++ch) {
            CircuitPath& P = lanes[L].path[s][fam][ch];
            P.apply(warm.values[s][fam][ch]);
            P.solver().setState(warm.path[s][fam][ch].data());
            P.bplusRef() = warm.bplus[s][fam][ch];
            P.setStepSize(h);
            P.solver().setMaxNewton(newtonCap);
            P.solver().resetFailureCount();
            const CircuitValues v = valuesFor(s, ch, Family(fam));
            lanes[L].applied[s][fam][ch] = v;
            P.apply(v);
        }
    }
}

void CircuitEngine::Impl::startQuality(int fi) noexcept {
    const int B = 1 - laneA;
    Lane& LB = lanes[B];
    LB.fi = fi;
    LB.on = true;
    const double h = 1.0 / (fs * kFactorList[fi]);
    for (int s = 0; s < kSlots; ++s) {
        const int fam = slot[s].fam;
        for (int ch = 0; ch < nCh; ++ch) {
            CircuitPath& src = lanes[laneA].path[s][fam][ch];
            CircuitPath& dst = LB.path[s][fam][ch];
            src.solver().getState(stateScratch.data());
            LB.applied[s][fam][ch] = lanes[laneA].applied[s][fam][ch];
            dst.apply(LB.applied[s][fam][ch]);
            dst.solver().setState(stateScratch.data());
            dst.setStepSize(h);
            dst.solver().setMaxNewton(newtonCap);
            dst.solver().resetFailureCount();
            dst.railRef() = src.railVolts();
            dst.bplusRef() = src.localBplus();
            dst.refreshLoads();
        }
    }
    // Warm the new oversamplers from recent history (FIR state becomes exact after its length).
    OsBank& b = bank[fi];
    const int F = kFactorList[fi];
    for (int ch = 0; ch < nCh; ++ch) {
        b.up[ch].reset();
        b.clipUp[ch].reset();
        b.clipDown[ch].reset();
        b.clipHiPad[ch].clear();
        b.clipBasePad[ch].clear();
        for (int s = 0; s < kSlots; ++s) { b.down[s][ch].reset(); b.hiPad[s][ch].clear(); b.basePad[s][ch].clear(); }
        for (int k = kHistory - 1; k >= 0; --k) {
            const double xv = hist[ch].tap(k);
            b.up[ch].up(&xv, hiTmp, 1);
            const double pv = preHist[ch].tap(k);
            b.clipUp[ch].up(&pv, hiTmp, 1);
            for (int j = 0; j < F; ++j) hiTmp[j] = b.clipHiPad[ch].process(dsp::softClip(hiTmp[j]));
            double y;
            b.clipDown[ch].down(hiTmp, &y, 1);
            b.clipBasePad[ch].process(y);
        }
    }
    qState = 1;
    qPos = 0;
}

// ---- per-sample circuit region -----------------------------------------------------------------

void CircuitEngine::Impl::circuitSample(int i, int channels, double extA, double preserve) noexcept {
    const double qw = qState ? laneWeight(qPos + i) : 0.0;
    const int laneB = 1 - laneA;
    for (int s = 0; s < kSlots; ++s) {
        Slot& S = slot[s];
        const double mains = S.mains.next(S.tMains), capF = S.railCap.next(S.tRailCapF), sag = S.sag.next(S.tSag);
        S.age.next(S.tAge); S.tol.next(S.tTol); S.leak.next(S.tLeak); S.ambient.next(S.tAmbient); S.mass.next(S.tMass);
        S.drift.next(S.tDrift);
        const double hz = S.humHz.next(S.tHumHz), drive = S.drive.next(S.tDrive), bias = S.bias.next(S.tBias);
        const double micro = S.micro.next(S.tMicro);
        const double step20 = 1.0 / fade20;
        if (S.rectW < 1.0) S.rectW = std::min(1.0, S.rectW + step20);
        if (S.coreW < 1.0) S.coreW = std::min(1.0, S.coreW + step20);
        if (S.resW < 1.0) S.resW = std::min(1.0, S.resW + step20);
        const RectifierLaw law = blendRectifier(rectifierLaw(S.rectPrev), rectifierLaw(S.rect), S.rectW);
        const FamilyNominal& na = nominal[S.fam];
        const double vNom = (na.vFiltPu[S.rectPrev] + S.rectW * (na.vFiltPu[S.rect] - na.vFiltPu[S.rectPrev])) * mains / 117.0;
        double vPu = S.supply.filter();
        if (vPu < vNom) vPu += std::clamp(preserve, 0.0, 1.0) * (vNom - vPu);  // r_eff = r + P(1-r): headroom only
        // Microphonic mode (host rate), bounded to 5% of the bias span.
        {
            const double z = std::clamp(S.micB1 * S.micZ1 + S.micB2 * S.micZ2 + S.micG * excite[i], -4.0, 4.0);
            S.micZ2 = S.micZ1; S.micZ1 = z;
        }
        const double micFrac = micro * 0.05 * std::clamp(S.micZ1, -1.0, 1.0);
        const double bN = std::clamp(bias + fin(S.mod.biasOffset, -1, 1, 0), -1.0, 1.0);
        const double gDrive = dbToLin(drive + fin(S.mod.driveOffsetDb, -24, 36, 0));
        const double k = S.tempcoNow;
        double iSumPu = 0;
        for (int Lsel = 0; Lsel < 2; ++Lsel) {
            const int L = Lsel == 0 ? laneA : laneB;
            if (Lsel == 1 && !qState) break;
            const double wl = Lsel == 0 ? (qState ? 1.0 - qw : 1.0) : qw;
            const int F = kFactorList[lanes[L].fi];
            for (int fsel = 0; fsel < 2; ++fsel) {
                const int fam = fsel == 0 ? S.fam : S.famNext;
                if (fam < 0) continue;
                const double fw = S.famNext < 0 ? 1.0 : famWeight(S.famPos + i);
                const double wf = fsel == 0 ? (S.famNext < 0 ? 1.0 : 1.0 - fw) : fw;
                const FamilyDesign& d = familyDesign(Family(fam));
                const double gin = gDrive * d.inVoltsPerFs, vRail = vPu * d.railDesignV, vc = complianceVolts(d);
                double vBias = (bN + micFrac) * d.biasSpanV;
                if (isTube(fam)) vBias -= 0.002 * k * (S.temps[0][int(ComponentId::V1)] - S.tnom[int(ComponentId::V1)]);
                const double os = nominal[fam].outScale * wf;
                const double wAll = wl * wf / F;
                for (int ch = 0; ch < channels; ++ch) {
                    CircuitPath& P = lanes[L].path[s][fam][ch];
                    P.setRail(vRail);
                    P.setBias(vBias);
                    const double* hin = &hiIn[L][ch][std::size_t(i) * F];
                    double* hout = &hiOut[L][s][ch][std::size_t(i) * F];
                    double iAcc = 0, pAcc[kComponentCount] = {};
                    for (int j = 0; j < F; ++j) {
                        P.setDrive(sourceCompliance(hin[j] * gin, vc));
                        bool ok;
                        if (forceFail) { ok = false; if (consecutiveFail < 3) ++consecutiveFail; }
                        else { ok = P.step(); }
                        if (!ok) {
                            ++solverFailures;
                            if ((forceFail && consecutiveFail >= 3) || P.solver().needsFallback()) enterFallback();
                        }
                        hout[j] += os * P.outputVolts();
                        iAcc += P.supplyCurrent();
                        P.accumulatePower(pAcc);
                    }
                    iSumPu += wAll * iAcc / d.designA;
                    for (int c = 0; c < kPerCh; ++c) S.power[thermalNode(ch, c)] += wAll * pAcc[c];
                    S.power[thermalNode(ch, int(ComponentId::V1))] += wl * wf * d.heaterW;
                    if (Lsel == 0 && fsel == 0) {
                        S.stageI[ch] = iAcc / F;
                        S.flux[ch] = P.flux();
                        S.hField[ch] = turnsPerLength(Family(fam)) * (P.magnetizingCurrent() + P.coreLossCurrent());
                    }
                }
            }
        }
        // Supply: physical stage draw + downstream proxy + creative augmentation (02 precedence).
        const FamilyDesign& da = familyDesign(Family(S.fam));
        const double iIdlePu = channels * na.iIdle / da.designA;
        const double iDyn = std::max(0.0, iSumPu - iIdlePu);
        const double auxIdle = std::max(0.0, 1.0 - nCh * na.iIdle / da.designA);
        const double physDyn = (1.0 + sag * da.auxGain) * iDyn;   // I_dyn incl. downstream proxy
        double iAux = auxIdle + sag * da.auxGain * iDyn;
        if (S.targets.sidechainMode != 1) iAux += S.targets.sagAugmentation * physDyn;
        if (S.targets.sidechainMode != 0 && std::isfinite(extA)) iAux += std::max(0.0, extA) / da.designA;
        const double dTres = S.temps[0][int(ComponentId::C_res)] - S.tnom[int(ComponentId::C_res)];
        const double cRes = capF * std::exp(-0.0085 * S.ageNow) * S.condValue[int(ComponentId::C_res)] *
                            std::max(0.5, 1.0 + 2.5e-3 * k * dTres) * (S.cond.c[int(ComponentId::C_res)].open ? 0.01 : 1.0);
        const double tRes = std::max(4.7e-6, cRes) * da.railDesignV / da.designA;
        double lf = std::clamp(S.leakNow + S.condLeak[int(ComponentId::C_res)], 0.0, 1.0);
        if (S.cond.c[int(ComponentId::C_res)].shortish) lf = 1.0;
        const double rLeak = std::exp(std::log(160.0) + lf * (std::log(5.0) - std::log(160.0)));
        const double kTarget = S.targets.recoveryTauS > 0
                                   ? std::clamp(Supply::naturalTau(tRes, law) / S.targets.recoveryTauS, 0.05, 20.0)
                                   : 1.0;
        S.kRec += (1.0 - std::exp(-1.0 / (fs * 0.05))) * (kTarget - S.kRec);
        S.supply.step(mains, hz, iSumPu, iAux, tRes, rLeak, law, S.kRec * S.condValue[int(ComponentId::D_rect)]);
        humPhase[s][i] = S.supply.phase();
        S.iDyn = iDyn;
        S.vStage = vPu * da.railDesignV;
        const double pScale = da.railDesignV * da.designA;
        S.power[thermalNode(0, int(ComponentId::D_rect))] += S.supply.rectifierLoss() * pScale + law.heaterW;
        S.power[thermalNode(0, int(ComponentId::C_res))] += S.supply.reservoirLoss() * pScale;
        S.power[thermalNode(0, int(ComponentId::R_filt))] += S.supply.filterResistorLoss() * pScale;
        S.thermal.accumulate(S.power);
        std::fill(std::begin(S.power), std::end(S.power), 0.0);
        if (S.thermal.advance()) {
            const auto& net = S.thermal.network();
            for (int ch = 0; ch < nCh; ++ch)
                for (int c = 0; c < kComponentCount; ++c) S.temps[ch][c] = net.temperature(std::size_t(thermalNode(ch, c)));
            refreshSlowState(s);
            applyValues(s);
        }
        S.railDc += (1.0 - std::exp(-1.0 / (fs * 0.05))) * (S.supply.reservoir() * da.railDesignV - S.railDc);
    }
}

// ---- slice --------------------------------------------------------------------------------------

void CircuitEngine::Impl::slice(const double* const* in, double* const* out, int channels, int off, int n,
                                const core::ModulationBlock& mod) noexcept {
    const int laneB = 1 - laneA;
    // 1. input gain, history, dry delays, transient detector, per-sample modulation arrays.
    for (int i = 0; i < n; ++i) {
        const double g = inGain.next(tInGain);
        double peak = 0;
        for (int ch = 0; ch < channels; ++ch) {
            double xr = in[ch][off + i];
            if (!std::isfinite(xr)) { xr = 0; ++nonFiniteIn; }
            raw[ch][i] = rawDelay[ch].process(xr);
            const double xg = xr * g;
            x[ch][i] = xg;
            dryAl[ch][i] = dryDelay[ch].process(xg);
            hist[ch].process(xg);
            peak = std::max(peak, std::fabs(xg));
            inPeak[ch] = std::max(inPeak[ch], std::fabs(xr));
            inSq[ch] += xr * xr;
        }
        detFast = peak > detFast ? aFastAtt * detFast + (1 - aFastAtt) * peak : aFastRel * detFast + (1 - aFastRel) * peak;
        detSlow = peak > detSlow ? aSlowAtt * detSlow + (1 - aSlowAtt) * peak : aSlowRel * detSlow + (1 - aSlowRel) * peak;
        double b = 0;
        if (detSlow > 1e-3) b = std::clamp((20.0 * std::log10((detFast + 1e-9) / (detSlow + 1e-9)) - 3.0) / 9.0, 0.0, 1.0);
        excite[i] = std::max(0.0, b - detPrev);
        detPrev = b;
        extLoad[i] = mod.externalLoadA ? mod.externalLoadA[off + i] : 0.0;
        preserveP[i] = mod.transientPreserve ? fin(mod.transientPreserve[off + i], 0, 1, 0) : 0.0;
    }
    // 2. upsample the input for every running lane; clear the high-rate accumulators.
    for (int Lsel = 0; Lsel < 2; ++Lsel) {
        const int L = Lsel == 0 ? laneA : laneB;
        if (Lsel == 1 && !qState) break;
        OsBank& b = bank[lanes[L].fi];
        const int F = kFactorList[lanes[L].fi];
        for (int ch = 0; ch < channels; ++ch) {
            b.up[ch].up(x[ch], hiIn[L][ch], n);
            for (int s = 0; s < kSlots; ++s) std::fill(hiOut[L][s][ch], hiOut[L][s][ch] + std::size_t(n) * F, 0.0);
        }
    }
    // 3. electrical solve, supply, power accumulation and due thermal ticks, per host sample.
    for (int i = 0; i < n; ++i) circuitSample(i, channels, extLoad[i], preserveP[i]);
    // 4. fractional (high-rate) pad -> downsample -> integer pad, per lane/slot/channel.
    for (int Lsel = 0; Lsel < 2; ++Lsel) {
        const int L = Lsel == 0 ? laneA : laneB;
        if (Lsel == 1 && !qState) break;
        OsBank& b = bank[lanes[L].fi];
        const int F = kFactorList[lanes[L].fi];
        for (int s = 0; s < kSlots; ++s)
            for (int ch = 0; ch < channels; ++ch) {
                double* h = hiOut[L][s][ch];
                if (b.hiPadLen > 0) for (int k = 0; k < n * F; ++k) h[k] = b.hiPad[s][ch].process(h[k]);
                b.down[s][ch].down(h, baseOut[L][s][ch], n);
                if (b.basePadLen > 0) for (int k = 0; k < n; ++k) baseOut[L][s][ch][k] = b.basePad[s][ch].process(baseOut[L][s][ch][k]);
            }
    }
    // 5. per-slot post (noise, LPF, shelf, pitch delay), A/B mix, level match, blend, trim, HPF.
    const double hn = std::sqrt(0.5 * (1 + 0.25 + 0.09 + 0.0225 + 0.0064));
    for (int i = 0; i < n; ++i) {
        const double qw = qState ? laneWeight(qPos + i) : 0.0;
        const double abW = ab.next(tAb), mixW = mix.next(tMix), og = outGain.next(tOutGain);
        const double hw = hpfOn.next(tHpfOn), hzv = hpfHz.next(tHpfHz), gw = gmOn.next(tGmOn);
        if (hzv != hpfApplied) {
            for (int ch = 0; ch < kMaxChannels; ++ch) hpf[ch].highpass(fs, hzv);
            hpfApplied = hzv;
        }
        double slotOut[kSlots][kMaxChannels] = {};
        for (int s = 0; s < kSlots; ++s) {
            Slot& S = slot[s];
            const double sh = S.shelf.next(S.tShelf);
            if (sh != S.shelfApplied) {
                for (auto& f : S.shelfF) { const double z1 = f.z1, z2 = f.z2; f.highShelf(fs, 6000.0, sh); f.z1 = z1; f.z2 = z2; }
                S.shelfApplied = sh;
            }
            const double lw = S.lpfOn.next(S.tLpfOn);
            const double lhz = core::applyCreativeCutoff(S.lpfHz.next(S.tLpfHz), S.targets.creativeCutoffOct, fs);
            if (lhz != S.lpfApplied) {
                for (auto& f : S.lpf) { const double z1 = f.z1, z2 = f.z2; f.lowpass(fs, lhz); f.z1 = z1; f.z2 = z2; }
                S.lpfApplied = lhz;
            }
            const double humL = S.humLevel.next(S.tHumLevel), jnL = S.jn.next(S.tJn);
            double hum = 0;
            if (humL > -120.0 + 1e-9) {
                const double ph = humPhase[s][i];
                for (int hk = 0; hk < 5; ++hk) hum += kHumHarm[hk] * std::sin((hk + 1) * ph);
                hum *= dbToLin(humL) / hn;
            }
            const double ageAmp = (analogNoise && S.targets.ageNoiseDbfs > -200.0) ? dbToLin(S.targets.ageNoiseDbfs) : 0.0;
            const ResistorTech& ra = kResTech[S.resPrev], &rb = kResTech[S.res];
            const double noiseDb = ra.noiseDb + S.resW * (rb.noiseDb - ra.noiseDb) + 0.3 * S.ageNow;
            // creative pitch-delay target (slew limited 0.002 s/s)
            if (latCreative > 0) {
                const double dev = S.targets.creativeDelayActive ? S.targets.creativeDelayMs * 1e-3 * fs : 0.0;
                const double target = std::clamp(latCreative + dev, 2.0, double(S.pitch[0].capacity() - 4));
                S.pitchDelay += std::clamp(target - S.pitchDelay, -0.002, 0.002);
            }
            for (int ch = 0; ch < channels; ++ch) {
                double v = qState ? (1.0 - qw) * baseOut[laneA][s][ch][i] + qw * baseOut[laneB][s][ch][i] : baseOut[laneA][s][ch][i];
                v += hum;
                if (jnL > -120.0 + 1e-9) {
                    const double tR = S.temps[ch][int(ComponentId::R_load)];
                    const double sigma = dbToLin(jnL + noiseDb) * std::sqrt(tR / kTref) * S.condNoise[int(ComponentId::R_load)];
                    v += sigma * S.jnRng[ch].gaussian();
                }
                if (ageAmp > 0) v += ageAmp * S.ageRng[ch].gaussian();
                const double yl = S.lpf[ch].process(v);
                v += lw * (yl - v);
                v = S.shelfF[ch].process(v);
                if (latCreative > 0) {
                    S.pitch[ch].process(v);
                    const double d = S.pitchDelay;
                    const int di = int(std::floor(d));
                    const double t = d - di;
                    const double y0 = S.pitch[ch].tap(di - 1), y1 = S.pitch[ch].tap(di), y2 = S.pitch[ch].tap(di + 1),
                                 y3 = S.pitch[ch].tap(di + 2);
                    // 4-point cubic Lagrange at fractional delay d (exact at integer d).
                    const double c1 = y2 - y0 / 3.0 - y1 / 2.0 - y3 / 6.0, c2 = 0.5 * (y0 + y2) - y1;
                    const double c3 = (y3 - y0) / 6.0 + 0.5 * (y1 - y2);
                    v = ((c3 * t + c2) * t + c1) * t + y1;
                }
                slotOut[s][ch] = v;
            }
        }
        double wet[kMaxChannels] = {}, dp = 0, wp = 0;
        for (int ch = 0; ch < channels; ++ch) {
            wet[ch] = (1.0 - abW) * slotOut[0][ch] + abW * slotOut[1][ch];
            if (injectNaN > 0) wet[ch] = std::numeric_limits<double>::quiet_NaN();
            dp += dryAl[ch][i] * dryAl[ch][i];
            wp += wet[ch] * wet[ch];
        }
        if (injectNaN > 0) --injectNaN;
        const double gmg = gm.updateMeanSquares(dp / std::max(1, channels), wp / std::max(1, channels));
        const double gEff = gw >= 1.0 ? gmg : std::exp(gw * std::log(gmg));
        for (int ch = 0; ch < channels; ++ch) {
            double y = (1.0 - mixW) * dryAl[ch][i] + mixW * gEff * wet[ch];
            y *= og;
            const double yh = hpf[ch].process(y);
            y += hw * (yh - y);
            pre[ch][i] = y;
            preHist[ch].process(y);
        }
        xfer[xferPos][0] = dryAl[0][i];
        xfer[xferPos][1] = wet[0];
        xferPos = (xferPos + 1) % kRingSize;
    }
    // 6. soft clip in its own oversampled stage (or native rate in zero-latency mode).
    for (int Lsel = 0; Lsel < 2; ++Lsel) {
        const int L = Lsel == 0 ? laneA : laneB;
        if (Lsel == 1 && !qState) break;
        for (int ch = 0; ch < channels; ++ch) {
            if (zeroLatency) {
                for (int k = 0; k < n; ++k) clipOut[L][ch][k] = dsp::softClip(pre[ch][k]);
                continue;
            }
            OsBank& b = bank[lanes[L].fi];
            const int F = kFactorList[lanes[L].fi];
            b.clipUp[ch].up(pre[ch], hiTmp, n);
            for (int k = 0; k < n * F; ++k) hiTmp[k] = dsp::softClip(hiTmp[k]);
            if (b.hiPadLen > 0) for (int k = 0; k < n * F; ++k) hiTmp[k] = b.clipHiPad[ch].process(hiTmp[k]);
            b.clipDown[ch].down(hiTmp, clipOut[L][ch], n);
            if (b.basePadLen > 0) for (int k = 0; k < n; ++k) clipOut[L][ch][k] = b.clipBasePad[ch].process(clipOut[L][ch][k]);
        }
    }
    // 7. clip enable crossfade, emergency containment, solver/containment fallback, host bypass.
    for (int i = 0; i < n; ++i) {
        const double qw = qState ? laneWeight(qPos + i) : 0.0;
        const double cw = clipOn.next(tClipOn);
        double yv[kMaxChannels];
        for (int ch = 0; ch < channels; ++ch) {
            const double yc = qState ? (1.0 - qw) * clipOut[laneA][ch][i] + qw * clipOut[laneB][ch][i] : clipOut[laneA][ch][i];
            const double yd = zeroLatency ? pre[ch][i] : clipDelay[ch].process(pre[ch][i]);
            double y = cw * yc + (1.0 - cw) * yd;
            if (!std::isfinite(y)) { y = 0.0; ++nonFiniteOut; enterFallback(); }
            else if (std::fabs(y) > 8.0) { y = std::copysign(8.0, y); ++clampedOut; enterFallback(); }
            yv[ch] = y;
        }
        const double fw = fallback.next(inFallback), bw = bypass.next(wantBypass);
        for (int ch = 0; ch < channels; ++ch) {
            double y = (1.0 - fw) * yv[ch] + fw * raw[ch][i];
            y = (1.0 - bw) * y + bw * raw[ch][i];
            out[ch][off + i] = y;
            outPeak[ch] = std::max(outPeak[ch], std::fabs(y));
            outSq[ch] += y * y;
        }
    }
    // 8. transitions and clocks.
    if (qState) {
        qPos += n;
        if (laneWeight(qPos - 1) >= 1.0) {
            lanes[laneA].on = false;
            laneA = laneB;
            activeFi = lanes[laneA].fi;
            qState = 0;
            qPos = 0;
        }
    }
    for (auto& S : slot)
        if (S.famNext >= 0) {
            S.famPos += n;
            if (famWeight(S.famPos - 1) >= 1.0) { S.fam = S.famNext; S.famNext = -1; S.famPos = 0; }
        }
    // Telemetry rings at the decimated cadence (end of slice is sufficient for display).
    ringCounter += n;
    while (ringCounter >= ringDecim) {
        ringCounter -= ringDecim;
        for (int s = 0; s < kSlots; ++s) {
            Slot& S = slot[s];
            const FamilyDesign& d = familyDesign(Family(S.fam));
            S.railRing[ringPos] = S.supply.reservoir() * d.railDesignV;
            S.bhRing[ringPos][0] = S.hField[0];
            S.bhRing[ringPos][1] = S.flux[0] / turnsArea(Family(S.fam));
        }
        ringPos = (ringPos + 1) % kRingSize;
    }
    levelCount += n;
    sampleIndex += n;
}

// ---- telemetry ----------------------------------------------------------------------------------

void CircuitEngine::Impl::publish(int channels) noexcept {
    TelemetryFrame& f = telemetry.back();
    f.generation = ++generation;
    f.sampleIndex = sampleIndex;
    f.sampleRate = fs;
    f.latencySamples = latTotal;
    f.activeFactor = kFactorList[activeFi];
    f.activeFamilyIir = iir ? 1 : 0;
    f.activeNewtonCap = newtonCap;
    f.activeZeroLatency = zeroLatency;
    f.activePitchDrift = pitchDrift;
    f.pendingFactor = kFactorList[qTargetFi];
    f.pendingFamilyIir = pendingIir ? 1 : 0;
    f.pendingZeroLatency = pendingZero;
    f.pendingPitchDrift = pendingPitch;
    f.reprepareRequired = pendingIir != iir || pendingZero != zeroLatency || pendingPitch != pitchDrift;
    f.qualityTransition = qState != 0;
    const double inv = levelCount > 0 ? 1.0 / double(levelCount) : 0.0;
    for (int ch = 0; ch < kMaxChannels; ++ch) {
        const bool on = ch < channels;
        f.inPeak[ch] = on ? inPeak[ch] : 0; f.inRms[ch] = on ? std::sqrt(inSq[ch] * inv) : 0;
        f.outPeak[ch] = on ? outPeak[ch] : 0; f.outRms[ch] = on ? std::sqrt(outSq[ch] * inv) : 0;
        inPeak[ch] = inSq[ch] = outPeak[ch] = outSq[ch] = 0;
    }
    levelCount = 0;
    f.solverFallback = inFallback;
    f.needsReset = needsReset;
    f.bypassed = wantBypass;
    f.hpfEnabled = tHpfOn;
    f.softClipEnabled = tClipOn;
    f.solverFailures = solverFailures;
    f.nonFiniteOutputs = nonFiniteOut;
    f.clampedOutputs = clampedOut;
    f.nonFiniteInputs = nonFiniteIn;
    f.ringDecimation = ringDecim;
    f.ringWrite = ringPos;
    f.gainMatch = gm.gain();
    for (int s = 0; s < kSlots; ++s) {
        const Slot& S = slot[s];
        SlotTelemetry& t = f.slot[s];
        const FamilyDesign& d = familyDesign(Family(S.fam));
        for (int ch = 0; ch < kMaxChannels; ++ch) {
            for (int c = 0; c < kComponentCount; ++c) {
                t.temperatureK[ch][c] = S.temps[std::min(ch, nCh - 1)][c];
                t.powerW[ch][c] = 0;
            }
            t.stageCurrentA[ch] = S.stageI[ch];
            t.fluxWb[ch] = S.flux[ch];
            t.fieldH[ch] = S.hField[ch];
            t.fluxDensityT[ch] = S.flux[ch] / turnsArea(Family(S.fam));
        }
        for (int ch = 0; ch < nCh; ++ch)
            for (int c = 0; c < kPerCh; ++c) {
                const CircuitPath& P = lanes[laneA].path[s][S.fam][ch];
                double p[kComponentCount] = {};
                P.accumulatePower(p);
                t.powerW[ch][c] = p[c] + (c == int(ComponentId::V1) ? d.heaterW : 0.0);
            }
        t.railVolts = S.supply.reservoir() * d.railDesignV;
        t.railDcVolts = S.railDc;
        t.railNominalVolts = nominal[S.fam].vResPu[S.rect] * d.railDesignV;
        t.family = S.fam;
        t.transitionFamily = S.famNext;
        std::copy(&S.railRing[0], &S.railRing[0] + kRingSize, t.railRing);
        std::copy(&S.bhRing[0][0], &S.bhRing[0][0] + 2 * kRingSize, &t.bhRing[0][0]);
    }
    std::copy(&xfer[0][0], &xfer[0][0] + 2 * kRingSize, &f.transferRing[0][0]);
    f.transferWrite = xferPos;
    telemetry.publish();
}

// ---- reset --------------------------------------------------------------------------------------

void CircuitEngine::Impl::doReset(ResetKind kind) noexcept {
    const Snapshot& snap = kind == ResetKind::Warm ? warm : cold;
    lanes[1 - laneA].on = false;
    lanes[laneA].on = true;
    qState = 0;
    qPos = 0;
    const double h = 1.0 / (fs * kFactorList[activeFi]);
    for (int s = 0; s < kSlots; ++s) {
        Slot& S = slot[s];
        S.famNext = -1;
        S.famPos = 0;
        for (int fam = 0; fam < kFamilyCount; ++fam)
            for (int ch = 0; ch < nCh; ++ch) {
                CircuitPath& P = lanes[laneA].path[s][fam][ch];
                P.apply(snap.values[s][fam][ch]);
                lanes[laneA].applied[s][fam][ch] = snap.values[s][fam][ch];
                P.solver().setState(snap.path[s][fam][ch].data());
                P.bplusRef() = snap.bplus[s][fam][ch];
                P.setStepSize(h);
                P.solver().setMaxNewton(newtonCap);
                P.solver().resetFailureCount();
                P.refreshLoads();
            }
        std::array<double, dsp::ThermalNetwork::kMaxNodes> tn{};
        for (int ch = 0; ch < kMaxChannels; ++ch)
            for (int c = 0; c < kComponentCount; ++c) S.temps[ch][c] = snap.temps[s][ch][c];
        for (int ch = 0; ch < nCh; ++ch)
            for (int c = 0; c < kComponentCount; ++c) tn[std::size_t(thermalNode(ch, c))] = S.temps[ch][c];
        S.thermal.network().setState(tn.data());
        S.thermal.clear();
        S.supply.setState(snap.supply[s][0], snap.supply[s][1], snap.supply[s][2]);
        for (int ch = 0; ch < kMaxChannels; ++ch) {
            S.jnRng[ch].seed(cfg.seed, uint64_t(s), uint64_t(ch), 2);
            S.ageRng[ch].seed(cfg.seed, uint64_t(s), uint64_t(ch), 3);
            S.lpf[ch].reset();
            S.shelfF[ch].reset();
            S.pitch[ch].clear();
        }
        S.pitchDelay = latCreative;
        S.micZ1 = S.micZ2 = 0;
        S.mains.snap(S.tMains); S.railCap.snap(S.tRailCapF); S.sag.snap(S.tSag); S.age.snap(S.tAge); S.tol.snap(S.tTol);
        S.leak.snap(S.tLeak); S.ambient.snap(S.tAmbient); S.mass.snap(S.tMass); S.drift.snap(S.tDrift); S.drive.snap(S.tDrive);
        S.bias.snap(S.tBias); S.humHz.snap(S.tHumHz); S.humLevel.snap(S.tHumLevel); S.jn.snap(S.tJn); S.micro.snap(S.tMicro);
        S.lpfHz.snap(S.tLpfHz); S.shelf.snap(S.tShelf); S.lpfOn.w = S.tLpfOn ? 1.0 : 0.0;
        S.rectPrev = S.rect; S.corePrev = S.core; S.resPrev = S.res;
        S.rectW = S.coreW = S.resW = 1.0;
        S.kRec = S.targets.recoveryConductanceScale;
        std::fill(std::begin(S.power), std::end(S.power), 0.0);
        for (int c = 0; c < kComponentCount; ++c) {
            const ComponentCondition& cc = S.cond.c[c];
            S.condValue[c] = fin(cc.valueScale, 0.05, 20.0, 1.0);
            S.condEsr[c] = fin(cc.esrScale, 1.0, 1000.0, 1.0);
            S.condNoise[c] = std::max(1e-6, fin(cc.noiseScale, 0.0, 100.0, 1.0));
            S.condLeak[c] = fin(cc.leakage, 0.0, 1.0, 0.0);
        }
        refreshSlowState(s);
        S.thermal.network().setAmbient(S.tAmbient);
        S.railDc = S.supply.reservoir() * familyDesign(Family(S.fam)).railDesignV;
        S.stageI[0] = S.stageI[1] = 0;
    }
    for (auto& b : bank) {
        if (!b.ok) continue;
        for (int ch = 0; ch < kMaxChannels; ++ch) {
            b.up[ch].reset(); b.clipUp[ch].reset(); b.clipDown[ch].reset();
            b.clipHiPad[ch].clear(); b.clipBasePad[ch].clear();
            for (int s = 0; s < kSlots; ++s) { b.down[s][ch].reset(); b.hiPad[s][ch].clear(); b.basePad[s][ch].clear(); }
        }
    }
    for (int ch = 0; ch < kMaxChannels; ++ch) {
        dryDelay[ch].clear(); rawDelay[ch].clear(); clipDelay[ch].clear(); hist[ch].clear(); preHist[ch].clear();
        hpf[ch].reset();
    }
    gm.reset();
    inGain.snap(tInGain); outGain.snap(tOutGain); mix.snap(tMix); ab.snap(tAb); hpfHz.snap(tHpfHz);
    hpfOn.w = tHpfOn ? 1 : 0; clipOn.w = tClipOn ? 1 : 0; gmOn.w = tGmOn ? 1 : 0; bypass.w = wantBypass ? 1 : 0;
    fallback.w = 0;
    inFallback = needsReset = false;
    consecutiveFail = 0;
    injectNaN = 0;
    detFast = detSlow = detPrev = 0;
}

// ---- checkpoint ---------------------------------------------------------------------------------

namespace {
struct StateIo {
    enum Mode { Count, Save, Validate, Load } mode = Count;
    double* v = nullptr;
    const double* input = nullptr;
    std::size_t nv = 0, pos = 0;
    uint64_t* r = nullptr;
    const uint64_t* rngInput = nullptr;
    std::size_t nr = 0, rpos = 0;
    double* scratch = nullptr;
    std::size_t nScratch = 0;
    bool ok = true;
    bool reading() const noexcept { return mode == Validate || mode == Load; }
    double d(double& x, double lo = -std::numeric_limits<double>::max(),
             double hi = std::numeric_limits<double>::max()) noexcept {
        double candidate = x;
        if (mode == Save) {
            if (pos < nv) v[pos] = x; else ok = false;
        } else if (reading()) {
            if (pos < nv && std::isfinite(input[pos]) && input[pos] >= lo && input[pos] <= hi) {
                candidate = input[pos];
                if (mode == Load) x = candidate;
            } else ok = false;
        }
        ++pos;
        return candidate;
    }
    template <class I> I integer(I& x, double lo = -9e15 + 1, double hi = 9e15 - 1) noexcept {
        double t = double(x);
        t = d(t, lo, hi);
        if (reading()) {
            // The 9e15 ceiling stays below the exact-integer limit of binary64; narrower
            // integer types also need their own bounds before any narrowing conversion.
            if (t != std::floor(t) || std::fabs(t) >= 9e15 ||
                static_cast<long double>(t) < std::numeric_limits<I>::lowest() ||
                static_cast<long double>(t) > std::numeric_limits<I>::max()) {
                ok = false;
                return x;
            }
            if (mode == Load) x = I(t);
            return I(t);
        }
        return x;
    }
    bool b(bool& x) noexcept {
        double t = x ? 1.0 : 0.0;
        t = d(t, 0.0, 1.0);
        if (reading() && t != 0.0 && t != 1.0) ok = false;
        if (mode == Load && ok) x = t == 1.0;
        return t == 1.0;
    }
    uint64_t u(uint64_t& x) noexcept {
        uint64_t candidate = x;
        if (mode == Save) {
            if (rpos < nr) r[rpos] = x; else ok = false;
        } else if (reading()) {
            if (rpos < nr) {
                candidate = rngInput[rpos];
                if (mode == Load) x = candidate;
            } else ok = false;
        }
        ++rpos;
        return candidate;
    }
    void arr(double* p, std::size_t n) noexcept { for (std::size_t k = 0; k < n; ++k) d(p[k]); }
    // Objects with stateSize/getState/setState via scratch.
    template <class T> void obj(T& o, std::size_t n) noexcept {
        if (n > nScratch) { ok = false; pos += n; return; }
        if (mode == Save) o.getState(scratch);
        for (std::size_t k = 0; k < n; ++k) scratch[k] = d(scratch[k]);
        if (mode == Validate && ok && !o.isStateValid(scratch)) ok = false;
        if (mode == Load && ok && !o.setState(scratch)) ok = false;
    }
    void delay(DelayLine& dl) noexcept {
        auto& st = dl.storage();
        arr(st.data(), st.size());
        integer(dl.position(), 0, double(dl.capacity() - 1));
    }
    void sm(Smoother& s) noexcept { d(s.y); }
    void biquad(Biquad& q) noexcept { d(q.b0); d(q.b1); d(q.b2); d(q.a1); d(q.a2); d(q.z1); d(q.z2); }
    void rng(Rng& g) noexcept {
        uint64_t nonzero = 0;
        for (auto& w : g.s) nonzero |= u(w);
        if (reading() && nonzero == 0) ok = false;
    }
    void cap(CapValues& c) noexcept { d(c.c); d(c.esr); d(c.leak); }
    void values(CircuitValues& cv) noexcept {
        for (double* p : {&cv.rG, &cv.rTop, &cv.rLoad, &cv.rK, &cv.rDec, &cv.rScreen, &cv.rCore, &cv.rRefl, &cv.rWind, &cv.lM,
                          &cv.lambdaS, &cv.exponent})
            d(*p);
        cap(cv.cIn); cap(cv.cK); cap(cv.cOut); cap(cv.cDec); cap(cv.cScreen);
        arr(cv.dev, 8);
    }
};
}  // namespace

template <class Io>
void CircuitEngine::Impl::visit(Io& io) {
    io.integer(sampleIndex, 0);
    io.u(epoch);
    io.b(sawEpoch);
    const int savedLane = io.integer(laneA, 0, 1);
    const int savedQState = io.integer(qState, 0, 1);
    const long savedQPos = io.integer(qPos, 0, warmup + fade20 + kSlice);
    const int savedActiveFi = io.integer(activeFi, 0, 4);
    io.integer(newtonCap, 1, 12);
    if (io.reading() && io.ok && (!bank[savedActiveFi].ok || (savedQState == 0 && savedQPos != 0))) io.ok = false;
    io.b(inFallback); io.b(needsReset); io.integer(consecutiveFail, 0, 3); io.integer(injectNaN, 0);
    io.d(detFast, 0); io.d(detSlow, 0); io.d(detPrev, 0, 1);
    io.sm(inGain); io.sm(outGain); io.sm(mix); io.sm(ab); io.sm(hpfHz);
    for (Ramp* r : {&hpfOn, &clipOn, &gmOn, &bypass, &fallback}) io.d(r->w, 0, 1);
    io.d(hpfApplied);
    io.obj(gm, 3);
    for (int ch = 0; ch < kMaxChannels; ++ch) {
        io.biquad(hpf[ch]);
        io.delay(dryDelay[ch]); io.delay(rawDelay[ch]); io.delay(clipDelay[ch]); io.delay(hist[ch]); io.delay(preHist[ch]);
    }
    for (auto& b : bank) {
        if (!b.ok) continue;
        for (int ch = 0; ch < kMaxChannels; ++ch) {
            io.obj(b.up[ch], std::size_t(b.up[ch].stateSize()));
            io.obj(b.clipUp[ch], std::size_t(b.clipUp[ch].stateSize()));
            io.obj(b.clipDown[ch], std::size_t(b.clipDown[ch].stateSize()));
            io.delay(b.clipHiPad[ch]); io.delay(b.clipBasePad[ch]);
            for (int s = 0; s < kSlots; ++s) {
                io.obj(b.down[s][ch], std::size_t(b.down[s][ch].stateSize()));
                io.delay(b.hiPad[s][ch]); io.delay(b.basePad[s][ch]);
            }
        }
    }
    int laneIndex = 0;
    for (auto& L : lanes) {
        const bool savedOn = io.b(L.on);
        const int savedFi = io.integer(L.fi, 0, 4);
        if (io.reading() && io.ok && (!bank[savedFi].ok ||
            (laneIndex == savedLane && (!savedOn || savedFi != savedActiveFi)) ||
            (laneIndex != savedLane && savedOn != (savedQState != 0)))) io.ok = false;
        ++laneIndex;
        for (int s = 0; s < kSlots; ++s)
            for (int fam = 0; fam < kFamilyCount; ++fam)
                for (int ch = 0; ch < nCh; ++ch) {
                    CircuitPath& P = L.path[s][fam][ch];
                    io.values(L.applied[s][fam][ch]);
                    if (io.mode == Io::Load && io.ok) P.apply(L.applied[s][fam][ch]);
                    io.d(P.railRef());
                    io.d(P.bplusRef());
                    io.obj(P.solver(), std::size_t(P.solver().stateSize()));
                    if (io.mode == Io::Load && io.ok) {
                        if (!P.setStepSize(1.0 / (fs * kFactorList[L.fi]))) io.ok = false;
                        P.solver().setMaxNewton(newtonCap);
                        P.refreshLoads();
                    }
                }
    }
    for (int s = 0; s < kSlots; ++s) {
        Slot& S = slot[s];
        for (Smoother* sm : {&S.mains, &S.railCap, &S.sag, &S.age, &S.tol, &S.leak, &S.ambient, &S.mass, &S.drift, &S.drive, &S.bias,
                             &S.humHz, &S.humLevel, &S.jn, &S.micro, &S.lpfHz, &S.shelf})
            io.sm(*sm);
        io.d(S.lpfOn.w, 0, 1);
        io.integer(S.fam, 0, kFamilyCount - 1);
        const int savedNext = io.integer(S.famNext, -1, kFamilyCount - 1);
        const long savedFamPos = io.integer(S.famPos, 0, fade20 + kSlice);
        if (io.reading() && savedNext == -1 && savedFamPos != 0) io.ok = false;
        io.integer(S.rect, 0, 2); io.integer(S.rectPrev, 0, 2);
        io.integer(S.core, 0, 2); io.integer(S.corePrev, 0, 2);
        io.integer(S.res, 0, 2); io.integer(S.resPrev, 0, 2);
        io.d(S.rectW, 0, 1); io.d(S.coreW, 0, 1); io.d(S.resW, 0, 1);
        auto* supply = S.supply.state();
        io.d(supply[0], 0); io.d(supply[1], 0);
        io.d(supply[2], 0, std::nextafter(2 * kPi, 0.0));
        io.d(supply[3], 0);
        const double conducting = io.d(supply[4], 0, 1);
        if (io.reading() && conducting != 0.0 && conducting != 1.0) io.ok = false;
        io.d(S.thermal.phase(), 0, std::nextafter(fs, 0.0));
        const double thermalCount = io.d(S.thermal.count(), 0, std::ceil(fs / ThermalClock::kRateHz));
        if (io.reading() && thermalCount != std::floor(thermalCount)) io.ok = false;
        io.arr(S.thermal.accumulator(), S.thermal.size());
        {
            const std::size_t n = S.thermal.size();
            if (io.mode == Io::Save) for (std::size_t k = 0; k < n; ++k) io.scratch[k] = S.thermal.network().temperature(k);
            for (std::size_t k = 0; k < n; ++k) io.scratch[k] = io.d(io.scratch[k]);
            if (io.mode == Io::Validate && io.ok && !S.thermal.network().isStateValid(io.scratch)) io.ok = false;
            if (io.mode == Io::Load && io.ok && !S.thermal.network().setState(io.scratch)) io.ok = false;
        }
        io.arr(&S.temps[0][0], std::size_t(kMaxChannels * kComponentCount));
        for (auto& cc : S.cond.c) { io.d(cc.valueScale); io.d(cc.leakage); io.d(cc.esrScale); io.d(cc.noiseScale); io.b(cc.open); io.b(cc.shortish); }
        io.arr(S.condValue, kComponentCount); io.arr(S.condEsr, kComponentCount); io.arr(S.condLeak, kComponentCount);
        io.arr(S.condNoise, kComponentCount);
        io.d(S.ageNow); io.d(S.sigmaNow); io.d(S.leakNow); io.d(S.tempcoNow); io.d(S.fluxNow);
        for (int ch = 0; ch < kMaxChannels; ++ch) {
            io.rng(S.jnRng[ch]); io.rng(S.ageRng[ch]);
            io.biquad(S.lpf[ch]); io.biquad(S.shelfF[ch]);
            io.delay(S.pitch[ch]);
        }
        io.d(S.micZ1); io.d(S.micZ2); io.d(S.lpfApplied); io.d(S.shelfApplied);
        io.d(S.pitchDelay, latCreative > 0 ? 2.0 : 0.0,
             latCreative > 0 ? double(S.pitch[0].capacity() - 4) : 0.0);
        io.d(S.kRec, 0.05, 20); io.d(S.railDc, 0);
        if (io.mode == Io::Load && io.ok) {
            if (!S.thermal.network().setAmbient(S.ambient.y) ||
                !S.thermal.network().setCapacityScale(0.25 * std::pow(16.0, std::clamp(S.mass.y, 0.0, 1.0)))) io.ok = false;
        }
    }
}

uint64_t CircuitEngine::Impl::layoutHash() const noexcept {
    uint64_t h = 1469598103934665603ull;
    auto mixIn = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
    uint64_t bits;
    std::memcpy(&bits, &fs, sizeof bits);
    mixIn(bits);
    mixIn(uint64_t(nCh)); mixIn(zeroLatency); mixIn(iir); mixIn(pitchDrift); mixIn(uint64_t(latTotal));
    mixIn(cfg.seed); mixIn(cfg.variationSeed[0]); mixIn(cfg.variationSeed[1]);
    // Covers both the serialized shape and continuation algorithms. Revision 2
    // admits cutoff modulation from the sample-rate-limited base frequency.
    mixIn(0xC0DE0002ull);
    return h;
}

// ---- public API -----------------------------------------------------------------------------------

CircuitEngine::CircuitEngine() = default;
CircuitEngine::~CircuitEngine() = default;

bool CircuitEngine::prepare(const EngineConfig& cfg, const core::ParamValues& plain, std::string& err) {
    auto fresh = std::make_unique<Impl>();
    if (!fresh->prepareAll(cfg, plain, err)) return false;
    StateIo count;
    count.scratch = fresh->stateScratch.data();
    count.nScratch = fresh->stateScratch.size();
    fresh->visit(count);
    if (!count.ok) { err = "checkpoint state exceeds prepared scratch capacity"; return false; }
    fresh->checkpointBackup.values.resize(count.pos);
    fresh->checkpointBackup.rng.resize(count.rpos);
    impl_ = std::move(fresh);
    err.clear();
    return true;
}

bool CircuitEngine::isPrepared() const noexcept { return impl_ && impl_->prepared; }
int CircuitEngine::latencySamples() const noexcept { return impl_ ? impl_->latTotal : 0; }

void CircuitEngine::process(const ProcessContext& ctx, const double* const* in, double* const* out, int channels,
                            const double* const* sidechain, int scChannels, int frames, const core::ParamValues& plain,
                            const core::ModulationBlock& mod) noexcept {
    (void)sidechain; (void)scChannels;
    if (frames <= 0) {
        if (frames == 0 && impl_ && impl_->prepared) {
            impl_->beginBlock(plain, mod, ctx);
            // Parameter-only host flushes can stage modes or complete a reset.
            // Publish their diagnostics without advancing any sample clock.
            impl_->publish(std::clamp(channels, 0, impl_->nCh));
        }
        return;
    }
    if (!impl_ || !impl_->prepared || !in || !out) {
        if (out) for (int ch = 0; ch < channels; ++ch) if (out[ch]) std::fill(out[ch], out[ch] + frames, 0.0);
        return;
    }
    Impl& m = *impl_;
    channels = std::clamp(channels, 0, m.nCh);
    if (channels == 0) return;
    m.beginBlock(plain, mod, ctx);
    for (int off = 0; off < frames; off += kSlice) m.slice(in, out, channels, off, std::min(kSlice, frames - off), mod);
    m.publish(channels);
}

void CircuitEngine::setBypassed(bool b) noexcept { if (impl_) impl_->wantBypass = b; }
void CircuitEngine::reset(ResetKind kind) noexcept { if (impl_ && impl_->prepared) impl_->doReset(kind); }

bool CircuitEngine::needsReprepare(const core::ParamValues& p) const noexcept {
    if (!impl_ || !impl_->prepared) return true;
    const Impl& m = *impl_;
    return (Impl::en(p[kPZeroLatency], 0, 1) != 0) != m.zeroLatency || (Impl::en(p[kPPhase], 0, 1) != 0) != m.iir ||
           (Impl::en(p[kPPitchDrift], 0, 1) != 0) != m.pitchDrift;
}

bool CircuitEngine::readTelemetry(TelemetryFrame& out) noexcept {
    if (!impl_) return false;
    impl_->telemetry.acquire();
    out = impl_->telemetry.front();
    return out.generation > 0;
}

bool CircuitEngine::submitConditions(int slot, const ConditionSet& set) noexcept {
    if (!impl_ || slot < 0 || slot >= kSlots) return false;
    impl_->conditions[slot].back() = set;
    impl_->conditions[slot].publish();
    return true;
}

std::size_t CircuitEngine::checkpointSize() const noexcept {
    if (!impl_) return 0;
    StateIo io;
    io.mode = StateIo::Count;
    io.scratch = impl_->stateScratch.data();
    io.nScratch = impl_->stateScratch.size();
    impl_->visit(io);
    return io.pos;
}

void CircuitEngine::allocateCheckpoint(Checkpoint& cp) const {
    if (!impl_) return;
    StateIo io;
    io.mode = StateIo::Count;
    io.scratch = impl_->stateScratch.data();
    io.nScratch = impl_->stateScratch.size();
    impl_->visit(io);
    cp.values.assign(io.pos, 0.0);
    cp.rng.assign(io.rpos, 0);
}

bool CircuitEngine::getCheckpoint(Checkpoint& cp) const noexcept {
    if (!impl_ || !impl_->prepared) return false;
    StateIo io;
    io.mode = StateIo::Save;
    io.v = cp.values.data(); io.nv = cp.values.size();
    io.r = cp.rng.data(); io.nr = cp.rng.size();
    io.scratch = impl_->stateScratch.data();
    io.nScratch = impl_->stateScratch.size();
    impl_->visit(io);
    if (io.pos != io.nv || io.rpos != io.nr) return false;
    cp.layout = impl_->layoutHash();
    cp.sampleIndex = impl_->sampleIndex;
    return io.ok;
}

bool CircuitEngine::setCheckpoint(const Checkpoint& cp) noexcept {
    if (!impl_ || !impl_->prepared || cp.layout != impl_->layoutHash()) return false;
    auto& backup = impl_->checkpointBackup;
    if (cp.values.size() != backup.values.size() || cp.rng.size() != backup.rng.size() ||
        cp.values.empty() || double(cp.sampleIndex) != cp.values[0]) return false;
    StateIo io;
    io.mode = StateIo::Validate;
    io.input = cp.values.data(); io.nv = cp.values.size();
    io.rngInput = cp.rng.data(); io.nr = cp.rng.size();
    io.scratch = impl_->stateScratch.data();
    io.nScratch = impl_->stateScratch.size();
    impl_->visit(io);
    if (!io.ok || io.pos != io.nv || io.rpos != io.nr) return false;

    // The first pass never writes live state. Keep a preallocated rollback image as well:
    // nested numerical setters also depend on circuit parameters restored earlier in the
    // second pass, and may refuse an otherwise finite state under those parameters.
    if (!getCheckpoint(backup)) return false;
    double oldAmbient[kSlots], oldCapacity[kSlots];
    for (int s = 0; s < kSlots; ++s) {
        oldAmbient[s] = impl_->slot[s].thermal.network().ambient();
        oldCapacity[s] = impl_->slot[s].thermal.network().capacityScale();
    }
    io.mode = StateIo::Load;
    io.pos = io.rpos = 0;
    impl_->visit(io);
    if (io.ok) return true;

    StateIo rollback;
    rollback.mode = StateIo::Load;
    rollback.input = backup.values.data(); rollback.nv = backup.values.size();
    rollback.rngInput = backup.rng.data(); rollback.nr = backup.rng.size();
    rollback.scratch = impl_->stateScratch.data();
    rollback.nScratch = impl_->stateScratch.size();
    for (int s = 0; s < kSlots; ++s) {
        auto& net = impl_->slot[s].thermal.network();
        net.setAmbient(oldAmbient[s]);
        net.setCapacityScale(oldCapacity[s]);
    }
    impl_->visit(rollback);
    return false;
}

EffectiveTargets CircuitEngine::effectiveTargets(int slot) const noexcept {
    if (!impl_ || slot < 0 || slot >= kSlots) return {};
    return impl_->slot[slot].targets;
}
int CircuitEngine::activeOversampling() const noexcept { return impl_ ? kFactorList[impl_->activeFi] : 0; }
int CircuitEngine::activeNewtonCap() const noexcept { return impl_ ? impl_->newtonCap : 0; }
bool CircuitEngine::qualityTransitionActive() const noexcept { return impl_ && impl_->qState != 0; }
bool CircuitEngine::solverFallbackActive() const noexcept { return impl_ && impl_->inFallback; }
void CircuitEngine::debugForceSolverFailure(bool on) noexcept { if (impl_) { impl_->forceFail = on; if (!on) impl_->consecutiveFail = 0; } }
void CircuitEngine::debugInjectWetNonFinite(int samples) noexcept { if (impl_) impl_->injectNaN = std::max(0, samples); }

}  // namespace cd::engine
