#include "control/HarmonicTarget.h"
#include <algorithm>
#include <cmath>

namespace cd::control {
namespace {
constexpr double kTwoPi = 6.283185307179586;
double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
}  // namespace

double HarmonicLevels::ratioDb() const noexcept {
    return 20.0 * std::log10((amplitude[2] + 1e-12) / (amplitude[3] + 1e-12));
}

HarmonicLevels measureHarmonics(const std::function<double(double)>& fn, double amplitude, int spc, int settle,
                                int measure) {
    HarmonicLevels r;
    if (!fn || !std::isfinite(amplitude) || amplitude <= 0.0 || spc < 32 || settle < 0 || measure < 1) return r;
    std::vector<double> c(static_cast<std::size_t>(spc)), s(static_cast<std::size_t>(spc));
    for (int i = 0; i < spc; ++i) {
        c[static_cast<std::size_t>(i)] = std::cos(kTwoPi * i / spc);
        s[static_cast<std::size_t>(i)] = std::sin(kTwoPi * i / spc);
    }
    for (int k = 0; k < settle * spc; ++k) {
        const double y = fn(amplitude * s[static_cast<std::size_t>(k % spc)]);
        if (!std::isfinite(y)) return r;
    }
    double re[10] = {}, im[10] = {}, dc = 0.0;
    const int total = measure * spc;
    for (int k = 0; k < total; ++k) {
        const int ph = k % spc;
        const double y = fn(amplitude * s[static_cast<std::size_t>(ph)]);
        if (!std::isfinite(y)) return r;
        dc += y;
        for (int h = 1; h <= 9; ++h) {
            const std::size_t idx = static_cast<std::size_t>((static_cast<long long>(h) * ph) % spc);
            re[h] += y * c[idx];
            im[h] += y * s[idx];
        }
    }
    r.dc = dc / total;
    for (int h = 1; h <= 9; ++h) r.amplitude[h] = 2.0 * std::sqrt(re[h] * re[h] + im[h] * im[h]) / total;
    r.ok = true;
    return r;
}

bool CalibrationMap::configure(double b0, double b1, double d0, double d1, MapProvenance p, std::string label) {
    if (!std::isfinite(b0) || !std::isfinite(b1) || !std::isfinite(d0) || !std::isfinite(d1) || !(b1 > b0) ||
        !(d1 > d0))
        return false;
    biasMin_ = b0;
    biasMax_ = b1;
    driveMin_ = d0;
    driveMax_ = d1;
    provenance_ = p;
    label_ = std::move(label);
    pts_.assign(static_cast<std::size_t>(kBiasPoints * kDrivePoints), MapPoint{});
    configured_ = true;
    return true;
}

bool CalibrationMap::setPoint(int i, int j, const MapPoint& pt) {
    if (!configured_ || i < 0 || i >= kBiasPoints || j < 0 || j >= kDrivePoints || !std::isfinite(pt.ratioDb) ||
        !std::isfinite(pt.dcRatio))
        return false;
    pts_[static_cast<std::size_t>(i * kDrivePoints + j)] = pt;
    return true;
}

bool CalibrationMap::usable() const noexcept {
    if (!configured_) return false;
    return std::any_of(pts_.begin(), pts_.end(), [](const MapPoint& p) { return p.stable; });
}
double CalibrationMap::biasAt(int i) const noexcept { return biasMin_ + (biasMax_ - biasMin_) * i / (kBiasPoints - 1); }
double CalibrationMap::driveDbAt(int j) const noexcept {
    return driveMin_ + (driveMax_ - driveMin_) * j / (kDrivePoints - 1);
}
const MapPoint& CalibrationMap::point(int i, int j) const noexcept {
    return pts_[static_cast<std::size_t>(i * kDrivePoints + j)];
}
int CalibrationMap::nearestDriveIndex(double db) const noexcept {
    const double f = (db - driveMin_) / (driveMax_ - driveMin_) * (kDrivePoints - 1);
    return static_cast<int>(clampd(std::floor(f + 0.5), 0.0, kDrivePoints - 1));
}

bool buildMap(CalibrationMap& map, const std::function<double(double, double, double)>& fn,
              const MapConditions& cond) {
    if (!fn || map.biasMax() <= map.biasMin()) return false;  // unconfigured map has a degenerate axis
    for (int i = 0; i < CalibrationMap::kBiasPoints; ++i) {
        for (int j = 0; j < CalibrationMap::kDrivePoints; ++j) {
            const double bias = map.biasAt(i), drive = map.driveDbAt(j);
            MapPoint pt;
            double sum = 0.0, worstDc = 0.0;
            bool good = true;
            for (const double lvl : cond.levelsDbfs) {
                const double amp = std::pow(10.0, lvl / 20.0);
                const auto lv = measureHarmonics([&](double x) { return fn(x, bias, drive); }, amp,
                                                 cond.samplesPerCycle);
                if (!lv.ok || lv.amplitude[1] <= 1e-12) { good = false; break; }
                sum += clampd(lv.ratioDb(), -cond.ratioClampDb, cond.ratioClampDb);
                worstDc = std::max(worstDc, std::fabs(lv.dc) / lv.amplitude[1]);
            }
            if (good) {
                pt.ratioDb = sum / 3.0;
                pt.dcRatio = worstDc;
                pt.stable = worstDc <= cond.maxDcRatio;
            }
            if (!map.setPoint(i, j, pt)) return false;
        }
    }
    return map.usable();
}

bool HarmonicTarget::configure(const CalibrationMap* map, double baseBias, double baseDrive, double fs) noexcept {
    enabled_ = false;
    map_ = nullptr;
    if (map == nullptr) { reason_ = DisabledReason::MissingMap; return false; }
    if (!map->usable()) { reason_ = DisabledReason::NoStablePoints; return false; }
    if (!std::isfinite(baseBias) || !std::isfinite(baseDrive) || !(fs >= 8000.0) || !std::isfinite(fs)) {
        reason_ = DisabledReason::InvalidBase;
        return false;
    }
    map_ = map;
    baseBias_ = clampd(baseBias, map->biasMin(), map->biasMax());
    baseDrive_ = baseDrive;
    coef_ = -std::expm1(-std::log(20.0) / (kSmoothSeconds * fs));
    buildBranch();
    if (!enabled_) return false;
    destBias_ = bias_ = baseBias_;
    destDrive_ = driveDb_ = baseDrive_;
    return true;
}

void HarmonicTarget::buildBranch() noexcept {
    constexpr int NB = CalibrationMap::kBiasPoints;
    const CalibrationMap& m = *map_;
    row_ = m.nearestDriveIndex(baseDrive_);
    const double f = (baseBias_ - m.biasMin()) / (m.biasMax() - m.biasMin()) * (NB - 1);
    const int baseCol = static_cast<int>(clampd(std::floor(f + 0.5), 0, NB - 1));
    // Enumerate maximal strictly monotone stable runs; pick the one containing the base column with
    // the largest R span (ties: lowest start).
    int bestA = -1, bestB = -1;
    double bestSpan = -1.0;
    int a = 0;
    while (a < NB) {
        if (!m.point(a, row_).stable) { ++a; continue; }
        int b = a;
        int dir = 0;
        while (b + 1 < NB && m.point(b + 1, row_).stable) {
            const double d = m.point(b + 1, row_).ratioDb - m.point(b, row_).ratioDb;
            const int sd = d > 1e-9 ? 1 : (d < -1e-9 ? -1 : 0);
            if (sd == 0 || (dir != 0 && sd != dir)) break;
            dir = sd;
            ++b;
        }
        if (b > a) {
            const double span = std::fabs(m.point(b, row_).ratioDb - m.point(a, row_).ratioDb);
            if (a <= baseCol && baseCol <= b && span > bestSpan + 1e-12) { bestA = a; bestB = b; bestSpan = span; }
        }
        a = (b > a) ? b : a + 1;  // adjacent runs share their turning column
    }
    if (bestA < 0) { reason_ = DisabledReason::NoMonotonicBranch; enabled_ = false; return; }
    b0_ = bestA;
    b1_ = bestB;
    const double r0 = m.point(b0_, row_).ratioDb, r1 = m.point(b1_, row_).ratioDb;
    rLo_ = std::min(r0, r1);
    rHi_ = std::max(r0, r1);
    // R at the base bias by linear interpolation inside its segment.
    const int k = std::min(std::max(static_cast<int>(std::floor(f)), b0_), b1_ - 1);
    const double u = clampd(f - k, 0.0, 1.0);
    rBase_ = m.point(k, row_).ratioDb * (1 - u) + m.point(k + 1, row_).ratioDb * u;
    enabled_ = true;
    reason_ = DisabledReason::None;
}

bool HarmonicTarget::setTarget(double target, double amount) noexcept {
    if (!enabled_ || !std::isfinite(target) || !std::isfinite(amount)) return false;
    const double t = clampd(target, -1.0, 1.0), amt = clampd(amount, 0.0, 1.0);
    const double rt = t >= 0.0 ? rBase_ + t * (rHi_ - rBase_) : rBase_ + t * (rBase_ - rLo_);
    const CalibrationMap& m = *map_;
    double bias = baseBias_;
    for (int k = b0_; k < b1_; ++k) {
        const double ra = m.point(k, row_).ratioDb, rb = m.point(k + 1, row_).ratioDb;
        const double lo = std::min(ra, rb), hi = std::max(ra, rb);
        if (rt >= lo - 1e-12 && rt <= hi + 1e-12) {
            const double u = (rb == ra) ? 0.0 : clampd((rt - ra) / (rb - ra), 0.0, 1.0);
            bias = m.biasAt(k) + u * (m.biasAt(k + 1) - m.biasAt(k));
            break;
        }
    }
    const double branchDrive = clampd(m.driveDbAt(row_), baseDrive_ - kMaxDriveDeltaDb, baseDrive_ + kMaxDriveDeltaDb);
    destBias_ = clampd(baseBias_ + amt * (bias - baseBias_), m.biasMin(), m.biasMax());
    destDrive_ = clampd(baseDrive_ + amt * (branchDrive - baseDrive_), baseDrive_ - kMaxDriveDeltaDb,
                        baseDrive_ + kMaxDriveDeltaDb);
    return true;
}

void HarmonicTarget::step() noexcept {
    if (!enabled_) return;
    bias_ += coef_ * (destBias_ - bias_);
    driveDb_ += coef_ * (destDrive_ - driveDb_);
}
void HarmonicTarget::advance(std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) step();
}

OperatingPoint HarmonicTarget::destination() const noexcept {
    OperatingPoint p;
    p.enabled = enabled_;
    p.reason = reason_;
    p.bias = destBias_;
    p.driveDb = destDrive_;
    return p;
}

}  // namespace cd::control
