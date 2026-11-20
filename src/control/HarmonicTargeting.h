// cd_control: harmonic targeting (CD-F14). Selects a calibrated bias/drive OPERATING POINT; it never
// adds synthetic harmonics and does not guarantee isolation of an order in arbitrary music.
//
// Data: HarmonicMap = 41 bias x 17 drive grid of R=20log10((H2+1e-12)/(H3+1e-12)) (clamped to +-60 dB)
// with per-point validity. Axes are in engine units: bias normalized (default -1..1, the slot bias
// parameter span) and drive in dB (default 0..36, the slot drive parameter range).
// A map is produced OFFLINE per model by calibrateHarmonicMap() against a render callback (the real
// engine at runtime; a labelled SYNTHETIC waveshaper in tests, G01 open). No map -> feature disabled.
//
// Selection: for each bias column take the valid drive row nearest the base drive inside +-6 dB;
// take the longest-span contiguous valid run; walk from its R minimum to its R maximum enforcing
// monotonicity (a safe monotonic branch); target -1..+1 interpolates R between the attainable endpoints
// (-1 Odd = low R, 0 Neutral = midpoint, +1 Even = high R). Output deltas (bias, drive dB) are
// amount * (point - base) with drive bounded to +-6 dB and bias inside the span, smoothed 50 ms.
//
// Threading: HarmonicMap/selection/HarmonicTargeting are noexcept, allocation-free, audio-safe (bounded
// 41x17 work). analyzeHarmonics()/calibrateHarmonicMap() allocate and are worker/offline only.
#pragma once
#include <cstddef>
#include <functional>
#include <vector>

namespace cd::control {

class HarmonicMap {
public:
    static constexpr int kBias = 41;
    static constexpr int kDrive = 17;
    static constexpr double kRClampDb = 60.0;

    HarmonicMap() noexcept { clear(); }
    void setAxes(double biasMin, double biasMax, double driveMinDb, double driveMaxDb) noexcept;
    void clear() noexcept;
    void set(int bi, int dj, double rDb, bool valid) noexcept;
    double r(int bi, int dj) const noexcept { return r_[bi][dj]; }
    bool valid(int bi, int dj) const noexcept { return ok_[bi][dj]; }
    double biasAt(int bi) const noexcept { return biasMin_ + (biasMax_ - biasMin_) * bi / double(kBias - 1); }
    double driveAt(int dj) const noexcept { return driveMin_ + (driveMax_ - driveMin_) * dj / double(kDrive - 1); }
    double biasMin() const noexcept { return biasMin_; }
    double biasMax() const noexcept { return biasMax_; }
    double driveMin() const noexcept { return driveMin_; }
    double driveMax() const noexcept { return driveMax_; }
    int validCount() const noexcept;

private:
    double biasMin_ = -1, biasMax_ = 1, driveMin_ = 0, driveMax_ = 36;
    double r_[kBias][kDrive];
    bool ok_[kBias][kDrive];
};

struct HarmonicLimits {
    double maxDriveDeltaDb = 6.0;
    double biasLo = -1.0, biasHi = 1.0;   // manifest bias span in axis units
    double minRSpanDb = 0.5;              // flatter maps are treated as unavailable
};

struct HarmonicSelection {
    bool available = false;
    double bias = 0, driveDb = 0;         // selected absolute operating point
    double rTarget = 0, rMin = 0, rMax = 0;
};

// target in [-1,+1] (-1 odd, +1 even). base values are absolute (same units as the map axes).
HarmonicSelection selectHarmonicTarget(const HarmonicMap& map, double target, double baseBias,
                                       double baseDriveDb, const HarmonicLimits& lim = {}) noexcept;

// Per-slot runtime: smoothed (bias, drive) deltas toward the selected point.
class HarmonicTargeting {
public:
    static constexpr double kSmoothS = 0.05;
    void setMap(const HarmonicMap* m) noexcept { map_ = m; }
    bool available() const noexcept { return map_ != nullptr; }
    // Block update. frames==0 passes dt=0 (state held). Returns false when disabled (zero deltas).
    bool update(double target, double amount, double baseBias, double baseDriveDb, double dtSeconds,
                const HarmonicLimits& lim = {}) noexcept;
    double biasDelta() const noexcept { return dBias_; }
    double driveDeltaDb() const noexcept { return dDrive_; }
    const HarmonicSelection& selection() const noexcept { return sel_; }
    void reset() noexcept { dBias_ = dDrive_ = 0; }

private:
    const HarmonicMap* map_ = nullptr;
    HarmonicSelection sel_{};
    double dBias_ = 0, dDrive_ = 0;
};

// --------------------------------------------------------------- analysis / calibration (offline)
struct HarmonicSpectrum {
    double h[10] = {0};        // linear amplitudes h[1]..h[9]; h[0] unused
    double dc = 0, peak = 0;
    bool finite = true;
    double ratioDb() const noexcept;  // R = 20log10((H2+1e-12)/(H3+1e-12))
};

// Hann-windowed Goertzel at k*f0 (k=1..9, skipped at/above Nyquist). Use >= 20 cycles.
HarmonicSpectrum analyzeHarmonics(const double* x, std::size_t n, double fs, double f0);

// Render callback: fill `out` with settled samples for the engine at the given operating point.
// Return false if the point cannot be rendered (treated as invalid).
using HarmonicRenderFn = std::function<bool(double bias, double driveDb, double f0Hz, double levelDbfs,
                                            std::vector<double>& out)>;

struct CalibrationConfig {
    double fs = 48000.0;
    std::vector<double> f0Hz{100.0, 1000.0};
    std::vector<double> levelsDbfs{-24.0, -12.0, -6.0};
    double dcLimitRel = 0.5;     // reject |DC| > dcLimitRel*H1 (DC-excess)
    double maxAbs = 8.0;         // reject runaway/unstable renders
    double minH1 = 1e-6;
};

// Fills `map` (axes must be set) with the mean R over all conditions; a point is valid only when every
// condition renders finite, bounded, DC-limited. Returns the number of valid points.
int calibrateHarmonicMap(HarmonicMap& map, const HarmonicRenderFn& render, const CalibrationConfig& cfg);

}  // namespace cd::control
