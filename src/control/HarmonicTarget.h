// HarmonicTarget (CD-F14): even/odd harmonic targeting via a calibrated bias/drive operating point.
// Owner module: cd_control. Threading: HarmonicMeasure/buildMap are OFFLINE (allocate, std::function);
// HarmonicTarget::setTarget()/step() are audio-thread safe once configured (no allocation).
// Selects an operating point from a measured map; it does not add synthetic harmonics. A map built
// from a synthetic callable is labelled Synthetic and is NOT a calibrated device map (gate G01 open).
#pragma once
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace cd::control {

struct HarmonicLevels {
    bool ok = false;
    double amplitude[10] = {};  // [1..9] linear peak amplitudes of harmonics (index 0 unused)
    double dc = 0.0;
    // R = 20 log10((H2+1e-12)/(H3+1e-12))
    double ratioDb() const noexcept;
};

// Drives `fn` with amplitude*sin at freq; samplesPerCycle must be >= 32. After `settleCycles`
// discarded cycles, measures `measureCycles` whole cycles by exact DFT bins for harmonics 1..9.
// Offline helper: works for any callable nonlinearity (may be stateful). Non-finite output => ok=false.
HarmonicLevels measureHarmonics(const std::function<double(double)>& fn, double amplitude, int samplesPerCycle = 256,
                                int settleCycles = 4, int measureCycles = 8);

struct MapPoint {
    double ratioDb = 0.0;  // mean R over conditions
    double dcRatio = 0.0;  // worst |DC| / H1 over conditions
    bool stable = false;   // finite, DC within limit, and all conditions measurable
};

enum class MapProvenance { Synthetic, Measured };

class CalibrationMap {
public:
    static constexpr int kBiasPoints = 41;
    static constexpr int kDrivePoints = 17;
    static constexpr double kDefaultMaxDcRatio = 0.25;  // proposed; calibrate under G01

    bool configure(double biasMin, double biasMax, double driveMinDb, double driveMaxDb, MapProvenance p,
                   std::string label);
    bool setPoint(int biasIdx, int driveIdx, const MapPoint& pt);
    // true if at least one stable point and the grid axes are valid
    bool usable() const noexcept;
    double biasAt(int i) const noexcept;
    double driveDbAt(int j) const noexcept;
    const MapPoint& point(int i, int j) const noexcept;
    int nearestDriveIndex(double driveDb) const noexcept;
    double biasMin() const noexcept { return biasMin_; }
    double biasMax() const noexcept { return biasMax_; }
    double driveMinDb() const noexcept { return driveMin_; }
    double driveMaxDb() const noexcept { return driveMax_; }
    MapProvenance provenance() const noexcept { return provenance_; }
    const std::string& label() const noexcept { return label_; }
    bool calibratedDevice() const noexcept { return provenance_ == MapProvenance::Measured; }

private:
    bool configured_ = false;
    double biasMin_ = 0, biasMax_ = 0, driveMin_ = 0, driveMax_ = 0;
    MapProvenance provenance_ = MapProvenance::Synthetic;
    std::string label_;
    std::vector<MapPoint> pts_;
};

struct MapConditions {
    double levelsDbfs[3] = {-24.0, -12.0, -6.0};
    int samplesPerCycle = 256;  // callable sees only x; frequency-dependent devices need a stateful wrapper
    double ratioClampDb = 60.0;  // R clamped to +/- this (noise-floor proxy)
    double maxDcRatio = CalibrationMap::kDefaultMaxDcRatio;
};

// fn(x, bias, driveDb) -> y. Offline. Builds 41x17 map; requires map.configure() already called.
bool buildMap(CalibrationMap& map, const std::function<double(double, double, double)>& fn,
              const MapConditions& cond = MapConditions{});

enum class DisabledReason { None, MissingMap, NoStablePoints, NoMonotonicBranch, InvalidBase };

struct OperatingPoint {
    bool enabled = false;
    DisabledReason reason = DisabledReason::MissingMap;
    double bias = 0.0;
    double driveDb = 0.0;
};

class HarmonicTarget {
public:
    static constexpr double kMaxDriveDeltaDb = 6.0;
    static constexpr double kSmoothSeconds = 0.050;  // 95% settling time

    // Map pointer must outlive use (or pass nullptr => disabled, MissingMap).
    bool configure(const CalibrationMap* map, double baseBias, double baseDriveDb, double sampleRate) noexcept;
    // target -1 Odd .. +1 Even; amount 0..1. Non-finite inputs rejected (state unchanged).
    bool setTarget(double target, double amount) noexcept;
    void step() noexcept;
    void advance(std::size_t n) noexcept;

    bool enabled() const noexcept { return enabled_; }
    DisabledReason reason() const noexcept { return reason_; }
    double bias() const noexcept { return bias_; }
    double driveDb() const noexcept { return driveDb_; }
    // Unsmoothed destination, for tests/UI.
    OperatingPoint destination() const noexcept;
    // Attainable R endpoints (dB) of the selected branch.
    double branchRatioMinDb() const noexcept { return rLo_; }
    double branchRatioMaxDb() const noexcept { return rHi_; }

private:
    void buildBranch() noexcept;
    const CalibrationMap* map_ = nullptr;
    bool enabled_ = false;
    DisabledReason reason_ = DisabledReason::MissingMap;
    double baseBias_ = 0, baseDrive_ = 0, coef_ = 0;
    int row_ = 0, b0_ = 0, b1_ = 0;  // branch at drive row_, bias columns b0_..b1_ (R strictly monotone)
    double rLo_ = 0, rHi_ = 0, rBase_ = 0;
    double destBias_ = 0, destDrive_ = 0, bias_ = 0, driveDb_ = 0;
};

}  // namespace cd::control
