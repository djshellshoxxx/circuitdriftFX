#include "control/HarmonicTargeting.h"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace cd::control {

void HarmonicMap::setAxes(double bmin, double bmax, double dmin, double dmax) noexcept {
    biasMin_ = bmin; biasMax_ = bmax; driveMin_ = dmin; driveMax_ = dmax;
}

void HarmonicMap::clear() noexcept {
    for (int i = 0; i < kBias; ++i)
        for (int j = 0; j < kDrive; ++j) { r_[i][j] = 0.0; ok_[i][j] = false; }
}

void HarmonicMap::set(int bi, int dj, double rDb, bool valid) noexcept {
    if (bi < 0 || bi >= kBias || dj < 0 || dj >= kDrive) return;
    const bool good = valid && std::isfinite(rDb);
    r_[bi][dj] = good ? std::clamp(rDb, -kRClampDb, kRClampDb) : 0.0;
    ok_[bi][dj] = good;
}

int HarmonicMap::validCount() const noexcept {
    int c = 0;
    for (int i = 0; i < kBias; ++i)
        for (int j = 0; j < kDrive; ++j) c += ok_[i][j] ? 1 : 0;
    return c;
}

HarmonicSelection selectHarmonicTarget(const HarmonicMap& map, double target, double baseBias,
                                       double baseDriveDb, const HarmonicLimits& lim) noexcept {
    HarmonicSelection out;
    constexpr int NB = HarmonicMap::kBias, ND = HarmonicMap::kDrive;
    const double t = std::isfinite(target) ? std::clamp(target, -1.0, 1.0) : 0.0;
    const double d0 = std::clamp(std::isfinite(baseDriveDb) ? baseDriveDb : 0.0, map.driveMin(), map.driveMax());
    out.bias = baseBias;
    out.driveDb = d0;

    int row[NB];
    double colR[NB];
    bool colOk[NB];
    for (int i = 0; i < NB; ++i) {
        colOk[i] = false; row[i] = -1; colR[i] = 0;
        const double b = map.biasAt(i);
        if (b < lim.biasLo - 1e-12 || b > lim.biasHi + 1e-12) continue;
        double bestD = 1e300;
        for (int j = 0; j < ND; ++j) {
            if (!map.valid(i, j)) continue;
            const double dd = std::fabs(map.driveAt(j) - d0);
            if (map.driveAt(j) < d0 - lim.maxDriveDeltaDb - 1e-9 || map.driveAt(j) > d0 + lim.maxDriveDeltaDb + 1e-9) continue;
            if (dd < bestD) { bestD = dd; row[i] = j; }
        }
        if (row[i] >= 0) { colOk[i] = true; colR[i] = map.r(i, row[i]); }
    }
    // Longest-span contiguous valid run.
    int bestLo = -1, bestHi = -1;
    double bestSpan = -1;
    for (int i = 0; i < NB;) {
        if (!colOk[i]) { ++i; continue; }
        int j = i;
        double mn = colR[i], mx = colR[i];
        while (j + 1 < NB && colOk[j + 1]) { ++j; mn = std::min(mn, colR[j]); mx = std::max(mx, colR[j]); }
        if (mx - mn > bestSpan) { bestSpan = mx - mn; bestLo = i; bestHi = j; }
        i = j + 1;
    }
    if (bestLo < 0 || bestSpan < lim.minRSpanDb) return out;

    int imin = bestLo, imax = bestLo;
    for (int i = bestLo; i <= bestHi; ++i) {
        if (colR[i] < colR[imin]) imin = i;
        if (colR[i] > colR[imax]) imax = i;
    }
    // Walk imin -> imax, monotone nondecreasing by cumulative-max clipping.
    const int step = imax >= imin ? 1 : -1;
    const int len = std::abs(imax - imin) + 1;
    double mono[NB];
    int idx[NB];
    double run = colR[imin];
    for (int k = 0; k < len; ++k) {
        const int i = imin + k * step;
        run = std::max(run, colR[i]);
        mono[k] = run;
        idx[k] = i;
    }
    out.rMin = mono[0];
    out.rMax = mono[len - 1];
    out.rTarget = out.rMin + 0.5 * (t + 1.0) * (out.rMax - out.rMin);
    int k = 0;
    while (k + 1 < len - 1 && mono[k + 1] < out.rTarget) ++k;
    double frac = 0.0;
    if (len > 1) {
        const double lo = mono[k], hi = mono[k + 1];
        frac = hi > lo ? std::clamp((out.rTarget - lo) / (hi - lo), 0.0, 1.0) : 0.0;
    }
    const int ia = idx[k], ib = idx[std::min(k + 1, len - 1)];
    out.bias = map.biasAt(ia) + frac * (map.biasAt(ib) - map.biasAt(ia));
    out.driveDb = map.driveAt(row[ia]) + frac * (map.driveAt(row[ib]) - map.driveAt(row[ia]));
    out.bias = std::clamp(out.bias, lim.biasLo, lim.biasHi);
    out.driveDb = std::clamp(out.driveDb, d0 - lim.maxDriveDeltaDb, d0 + lim.maxDriveDeltaDb);
    out.available = true;
    return out;
}

bool HarmonicTargeting::update(double target, double amount, double baseBias, double baseDriveDb,
                               double dt, const HarmonicLimits& lim) noexcept {
    if (!map_) {
        sel_ = HarmonicSelection{};
        dBias_ = dDrive_ = 0;
        return false;
    }
    sel_ = selectHarmonicTarget(*map_, target, baseBias, baseDriveDb, lim);
    const double a = std::isfinite(amount) ? std::clamp(amount, 0.0, 1.0) : 0.0;
    double tb = 0, td = 0;
    if (sel_.available) {
        tb = a * (sel_.bias - baseBias);
        td = a * (sel_.driveDb - baseDriveDb);
        td = std::clamp(td, -lim.maxDriveDeltaDb, lim.maxDriveDeltaDb);
    }
    if (dt > 0) {
        const double c = 1.0 - std::exp(-dt / kSmoothS);
        dBias_ += (tb - dBias_) * c;
        dDrive_ += (td - dDrive_) * c;
    }
    return sel_.available;
}

// ---------------------------------------------------------------- analysis
double HarmonicSpectrum::ratioDb() const noexcept { return 20.0 * std::log10((h[2] + 1e-12) / (h[3] + 1e-12)); }

HarmonicSpectrum analyzeHarmonics(const double* x, std::size_t n, double fs, double f0) {
    HarmonicSpectrum s;
    if (n < 16 || !(fs > 0) || !(f0 > 0)) { s.finite = false; return s; }
    constexpr double pi = std::numbers::pi;
    std::vector<double> xw(n);
    double wsum = 0, dc = 0, peak = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (!std::isfinite(x[i])) { s.finite = false; return s; }
        const double w = 0.5 - 0.5 * std::cos(2.0 * pi * double(i) / double(n));
        xw[i] = x[i] * w;
        wsum += w;
        dc += xw[i];
        peak = std::max(peak, std::fabs(x[i]));
    }
    s.dc = dc / wsum;
    s.peak = peak;
    for (int k = 1; k <= 9; ++k) {
        const double f = k * f0;
        if (f >= 0.5 * fs) { s.h[k] = 0; continue; }
        const double om = 2.0 * pi * f / fs;
        const double coeff = 2.0 * std::cos(om);
        double s1 = 0, s2 = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const double s0 = xw[i] + coeff * s1 - s2;
            s2 = s1; s1 = s0;
        }
        const double re = s1 - s2 * std::cos(om), im = s2 * std::sin(om);
        s.h[k] = 2.0 * std::sqrt(re * re + im * im) / wsum;
    }
    return s;
}

int calibrateHarmonicMap(HarmonicMap& map, const HarmonicRenderFn& render, const CalibrationConfig& cfg) {
    std::vector<double> buf;
    int valid = 0;
    for (int i = 0; i < HarmonicMap::kBias; ++i)
        for (int j = 0; j < HarmonicMap::kDrive; ++j) {
            double sum = 0;
            int cnt = 0;
            bool ok = true;
            for (double f0 : cfg.f0Hz) {
                for (double lvl : cfg.levelsDbfs) {
                    buf.clear();
                    if (!render || !render(map.biasAt(i), map.driveAt(j), f0, lvl, buf)) { ok = false; break; }
                    const HarmonicSpectrum sp = analyzeHarmonics(buf.data(), buf.size(), cfg.fs, f0);
                    if (!sp.finite || sp.peak > cfg.maxAbs || sp.h[1] < cfg.minH1 ||
                        std::fabs(sp.dc) > cfg.dcLimitRel * sp.h[1]) { ok = false; break; }
                    sum += sp.ratioDb();
                    ++cnt;
                }
                if (!ok) break;
            }
            ok = ok && cnt > 0;
            map.set(i, j, ok ? sum / cnt : 0.0, ok);
            valid += ok ? 1 : 0;
        }
    return valid;
}

}  // namespace cd::control
