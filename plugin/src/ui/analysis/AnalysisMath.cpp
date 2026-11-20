#include "ui/analysis/AnalysisMath.h"

#include <limits>

namespace cd::plugin::ui::analysis {

// ---- XY ------------------------------------------------------------------------------------------
XyMap makeEqualScaleMap(double x, double y, double w, double h, double fullScale) noexcept {
    XyMap m;
    m.cx = x + w * 0.5;
    m.cy = y + h * 0.5;
    m.pxPerUnit = 0.5 * std::min(w, h) / std::max(fullScale, 1e-12);
    return m;
}

void referenceLine(const XyMap& m, double gain, double fullScale, double& x0, double& y0, double& x1, double& y1) noexcept {
    x0 = m.px(-fullScale); y0 = m.py(-fullScale * gain);
    x1 = m.px(fullScale);  y1 = m.py(fullScale * gain);
}

double distanceToLine(double px, double py, double x0, double y0, double x1, double y1) noexcept {
    const double dx = x1 - x0, dy = y1 - y0;
    const double len = std::hypot(dx, dy);
    if (len < 1e-12) return std::hypot(px - x0, py - y0);
    return std::abs(dy * (px - x0) - dx * (py - y0)) / len;
}

double maxPixelErrorToReference(const XyMap& m, const double* in, const double* out, int n, double gain, double fullScale) noexcept {
    double x0, y0, x1, y1, worst = 0;
    referenceLine(m, gain, fullScale, x0, y0, x1, y1);
    for (int i = 0; i < n; ++i) worst = std::max(worst, distanceToLine(m.px(in[i]), m.py(out[i]), x0, y0, x1, y1));
    return worst;
}

double smallSignalSlope(const double* in, const double* out, int n, double window) noexcept {
    double sx = 0, sy = 0;
    int cnt = 0;
    for (int i = 0; i < n; ++i)
        if (std::abs(in[i]) <= window) { sx += in[i]; sy += out[i]; ++cnt; }
    if (cnt < 3) return std::numeric_limits<double>::quiet_NaN();
    const double mx = sx / cnt, my = sy / cnt;
    double sxx = 0, sxy = 0;
    for (int i = 0; i < n; ++i)
        if (std::abs(in[i]) <= window) { sxx += (in[i] - mx) * (in[i] - mx); sxy += (in[i] - mx) * (out[i] - my); }
    return sxx > 1e-18 ? sxy / sxx : std::numeric_limits<double>::quiet_NaN();
}

double pixelSlope(const XyMap& m, const double* in, const double* out, int n) noexcept {
    if (n < 2) return std::numeric_limits<double>::quiet_NaN();
    double mx = 0, my = 0;
    for (int i = 0; i < n; ++i) { mx += m.px(in[i]); my += m.py(out[i]); }
    mx /= n; my /= n;
    double sxx = 0, sxy = 0;
    for (int i = 0; i < n; ++i) {
        const double dx = m.px(in[i]) - mx, dy = m.py(out[i]) - my;
        sxx += dx * dx; sxy += dx * dy;
    }
    return sxx > 1e-18 ? -sxy / sxx : std::numeric_limits<double>::quiet_NaN();
}

void decimateOrderedExtrema(const double* x, const double* y, int n, int maxPoints, std::vector<double>& ox, std::vector<double>& oy) {
    ox.clear(); oy.clear();
    if (n <= 0) return;
    if (n <= maxPoints || maxPoints < 6) {
        for (int i = 0; i < n; ++i) { ox.push_back(x[i]); oy.push_back(y[i]); }
        return;
    }
    const int buckets = maxPoints / 3;
    for (int b = 0; b < buckets; ++b) {
        const int i0 = static_cast<int>(static_cast<long long>(n) * b / buckets);
        const int i1 = static_cast<int>(static_cast<long long>(n) * (b + 1) / buckets);
        if (i1 <= i0) continue;
        int imin = i0, imax = i0;
        for (int i = i0; i < i1; ++i) {
            if (y[i] < y[imin]) imin = i;
            if (y[i] > y[imax]) imax = i;
        }
        int idx[3] = {i0, imin, imax};
        std::sort(idx, idx + 3);
        int last = -1;
        for (int k = 0; k < 3; ++k) {
            if (idx[k] == last) continue;
            last = idx[k];
            ox.push_back(x[idx[k]]); oy.push_back(y[idx[k]]);
        }
    }
}

void unrollRing(const double* ring, int size, int write, double* out) noexcept {
    if (size <= 0) return;
    const int w = ((write % size) + size) % size;
    for (int k = 0; k < size; ++k) out[k] = ring[(w + k) % size];
}

// ---- Rail history ---------------------------------------------------------------------------------
RailHistory::RailHistory() : bins_(kBins), stamp_(kBins, -1) {}

void RailHistory::clear() {
    std::fill(stamp_.begin(), stamp_.end(), std::int64_t{-1});
    newest_ = -1;
}

bool RailHistory::ingest(std::int64_t sampleIndexNewest, double sampleRate, const double* ring, int ringWrite, int n, int spacingSamples) {
    if (n <= 0 || spacingSamples <= 0 || sampleRate <= 0 || ring == nullptr) return false;
    const double binWidth = sampleRate / 1000.0;
    bool cleared = false;
    const auto frameNewestBin = static_cast<std::int64_t>(std::floor(static_cast<double>(sampleIndexNewest) / binWidth));
    if (newest_ >= 0 && frameNewestBin < newest_ - 1000) { clear(); cleared = true; }

    std::int64_t curBin = -1;
    double mn = 0, mx = 0, sum = 0;
    int cnt = 0;
    auto flush = [&] {
        if (cnt == 0 || curBin < 0) return;
        const auto idx = static_cast<std::size_t>(curBin % kBins);
        stamp_[idx] = curBin;
        bins_[idx] = {static_cast<float>(mn), static_cast<float>(sum / cnt), static_cast<float>(mx), true};
        newest_ = std::max(newest_, curBin);
        cnt = 0;
    };
    const int w = ((ringWrite % n) + n) % n;
    for (int k = 0; k < n; ++k) {
        const double v = ring[(w + k) % n];
        const std::int64_t t = sampleIndexNewest - static_cast<std::int64_t>(n - 1 - k) * spacingSamples;
        // Exactly 0.0 marks a never-written ring slot (a real rail is never exactly 0 V).
        if (t < 0 || !std::isfinite(v) || v == 0.0) continue;
        const auto bin = static_cast<std::int64_t>(std::floor(static_cast<double>(t) / binWidth));
        if (bin != curBin) { flush(); curBin = bin; mn = mx = v; sum = 0; cnt = 0; }
        mn = std::min(mn, v); mx = std::max(mx, v); sum += v; ++cnt;
    }
    flush();
    return cleared;
}

bool RailHistory::binAt(std::int64_t absBin, RailBin& out) const noexcept {
    if (absBin < 0 || newest_ < 0 || absBin > newest_ || newest_ - absBin >= kBins) return false;
    const auto idx = static_cast<std::size_t>(absBin % kBins);
    if (stamp_[idx] != absBin) return false;
    out = bins_[idx];
    return true;
}

void decimateEnvelope(const RailHistory& h, std::int64_t t0Bin, std::int64_t t1Bin, int columns, std::vector<EnvelopeColumn>& out) {
    out.assign(static_cast<std::size_t>(std::max(columns, 0)), {});
    if (columns <= 0 || t1Bin <= t0Bin) return;
    const double span = static_cast<double>(t1Bin - t0Bin);
    for (int c = 0; c < columns; ++c) {
        auto b0 = t0Bin + static_cast<std::int64_t>(std::floor(span * c / columns));
        auto b1 = t0Bin + static_cast<std::int64_t>(std::floor(span * (c + 1) / columns));
        if (b1 <= b0) b1 = b0 + 1;
        EnvelopeColumn col;
        double sum = 0;
        int cnt = 0;
        for (auto b = b0; b < b1; ++b) {
            RailBin rb;
            if (!h.binAt(b, rb)) continue;
            if (cnt == 0) { col.vmin = rb.vmin; col.vmax = rb.vmax; }
            col.vmin = std::min(col.vmin, rb.vmin); col.vmax = std::max(col.vmax, rb.vmax);
            sum += rb.vmean; ++cnt;
        }
        if (cnt > 0) { col.vmean = static_cast<float>(sum / cnt); col.valid = true; }
        out[static_cast<std::size_t>(c)] = col;
    }
}

RecoveryResult measureRecovery(const RailHistory& h, std::int64_t t0Bin, std::int64_t t1Bin, double minDroopFraction) {
    RecoveryResult r;
    RailBin rb;
    bool found = false;
    double minV = 0;
    std::int64_t minBin = t0Bin;
    for (auto b = t0Bin; b < t1Bin; ++b) {
        if (!h.binAt(b, rb)) continue;
        if (!found || rb.vmean < minV - 1e-3) { minV = rb.vmean; minBin = b; found = true; }
    }
    if (!found) return r;
    bool havePre = false;
    double pre = 0;
    for (auto b = std::max<std::int64_t>(t0Bin, minBin - 300); b < minBin; ++b) {
        if (!h.binAt(b, rb)) continue;
        if (!havePre || rb.vmean > pre) { pre = rb.vmean; havePre = true; }
    }
    if (!havePre) return r;
    const double droop = pre - minV;
    r.minVolts = minV; r.preLevelVolts = pre; r.minBin = minBin;
    if (droop < minDroopFraction * std::abs(pre) || droop <= 0) return r;
    const double threshold = minV + 0.95 * droop;
    double hi = minV;
    r.status = RecoveryResult::Status::NotRecovered;
    for (auto b = minBin + 1; b < t1Bin; ++b) {
        if (!h.binAt(b, rb)) {
            // Data after the newest bin simply has not arrived yet; an interior gap is a rejection.
            if (b <= h.newestBin()) r.status = RecoveryResult::Status::RejectedGap;
            return r;
        }
        const double v = rb.vmean;
        if (v >= threshold) {
            r.status = RecoveryResult::Status::Recovered;
            r.recoveryMs = static_cast<double>(b - minBin);
            return r;
        }
        hi = std::max(hi, v);
        if (hi > minV + 0.2 * droop && v < hi - 0.2 * droop) { r.status = RecoveryResult::Status::RejectedOverlap; return r; }
    }
    return r;
}

// ---- FFT / cross spectrum ---------------------------------------------------------------------------
Fft::Fft(int size) : n_(size), tw_(static_cast<std::size_t>(size / 2)), rev_(static_cast<std::size_t>(size)) {
    int bits = 0;
    while ((1 << bits) < n_) ++bits;
    for (int i = 0; i < n_; ++i) {
        int r = 0;
        for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        rev_[static_cast<std::size_t>(i)] = r;
    }
    for (int k = 0; k < n_ / 2; ++k) tw_[static_cast<std::size_t>(k)] = std::polar(1.0, -2.0 * kPi * k / n_);
}

void Fft::forward(std::vector<std::complex<double>>& d) const {
    for (int i = 0; i < n_; ++i) {
        const int j = rev_[static_cast<std::size_t>(i)];
        if (j > i) std::swap(d[static_cast<std::size_t>(i)], d[static_cast<std::size_t>(j)]);
    }
    for (int len = 2; len <= n_; len <<= 1) {
        const int half = len / 2, step = n_ / len;
        for (int i = 0; i < n_; i += len)
            for (int k = 0; k < half; ++k) {
                const auto u = d[static_cast<std::size_t>(i + k)];
                const auto v = d[static_cast<std::size_t>(i + k + half)] * tw_[static_cast<std::size_t>(k * step)];
                d[static_cast<std::size_t>(i + k)] = u + v;
                d[static_cast<std::size_t>(i + k + half)] = u - v;
            }
    }
}

CrossSpectrumAnalyzer::CrossSpectrumAnalyzer(int fftSize, int hop, double sampleRate, double tauSeconds)
    : fftSize_(fftSize), hop_(hop), fs_(sampleRate), tau_(tauSeconds), fft_(fftSize),
      window_(static_cast<std::size_t>(fftSize)), ringA_(static_cast<std::size_t>(fftSize), 0.0),
      ringB_(static_cast<std::size_t>(fftSize), 0.0), xa_(static_cast<std::size_t>(fftSize)),
      xb_(static_cast<std::size_t>(fftSize)), sLR_(static_cast<std::size_t>(fftSize / 2 + 1)),
      sLL_(static_cast<std::size_t>(fftSize / 2 + 1)), sRR_(static_cast<std::size_t>(fftSize / 2 + 1)) {
    for (int i = 0; i < fftSize_; ++i) window_[static_cast<std::size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * kPi * i / fftSize_);
    ampScale_ = 4.0 / fftSize_;  // 2 / sum(hann)
}

void CrossSpectrumAnalyzer::reset() {
    frames_ = 0; pos_ = 0; count_ = 0; sinceHop_ = 0; elapsedSeconds_ = 0;
    std::fill(ringA_.begin(), ringA_.end(), 0.0);
    std::fill(ringB_.begin(), ringB_.end(), 0.0);
    std::fill(sLR_.begin(), sLR_.end(), std::complex<double>{});
    std::fill(sLL_.begin(), sLL_.end(), 0.0);
    std::fill(sRR_.begin(), sRR_.end(), 0.0);
}

void CrossSpectrumAnalyzer::processFrame(const double* a, const double* b, double dtSeconds) {
    for (int i = 0; i < fftSize_; ++i) {
        const double w = window_[static_cast<std::size_t>(i)];
        xa_[static_cast<std::size_t>(i)] = a[i] * w;
        xb_[static_cast<std::size_t>(i)] = b[i] * w;
    }
    fft_.forward(xa_);
    fft_.forward(xb_);
    const double alpha = frames_ == 0 ? 1.0 : 1.0 - std::exp(-std::max(dtSeconds, 0.0) / tau_);
    for (int k = 0; k <= fftSize_ / 2; ++k) {
        const auto ka = xa_[static_cast<std::size_t>(k)], kb = xb_[static_cast<std::size_t>(k)];
        const auto cross = ka * std::conj(kb);
        auto& lr = sLR_[static_cast<std::size_t>(k)];
        auto& ll = sLL_[static_cast<std::size_t>(k)];
        auto& rr = sRR_[static_cast<std::size_t>(k)];
        lr += alpha * (cross - lr);
        ll += alpha * (std::norm(ka) - ll);
        rr += alpha * (std::norm(kb) - rr);
    }
    if (frames_ > 0) elapsedSeconds_ += dtSeconds;
    ++frames_;
}

void CrossSpectrumAnalyzer::push(const double* a, const double* b, int n) {
    std::vector<double> fa(static_cast<std::size_t>(fftSize_)), fb(static_cast<std::size_t>(fftSize_));
    for (int i = 0; i < n; ++i) {
        ringA_[static_cast<std::size_t>(pos_)] = a[i];
        ringB_[static_cast<std::size_t>(pos_)] = b[i];
        pos_ = (pos_ + 1) % fftSize_;
        if (count_ < fftSize_) ++count_;
        if (++sinceHop_ >= hop_ && count_ >= fftSize_) {
            sinceHop_ = 0;
            for (int k = 0; k < fftSize_; ++k) {
                fa[static_cast<std::size_t>(k)] = ringA_[static_cast<std::size_t>((pos_ + k) % fftSize_)];
                fb[static_cast<std::size_t>(k)] = ringB_[static_cast<std::size_t>((pos_ + k) % fftSize_)];
            }
            processFrame(fa.data(), fb.data(), static_cast<double>(hop_) / fs_);
        }
    }
}

void CrossSpectrumAnalyzer::bins(std::vector<PhaseBin>& out) const {
    out.clear();
    out.reserve(static_cast<std::size_t>(fftSize_ / 2));
    for (int k = 1; k <= fftSize_ / 2; ++k) {
        const auto lr = sLR_[static_cast<std::size_t>(k)];
        const double ll = sLL_[static_cast<std::size_t>(k)], rr = sRR_[static_cast<std::size_t>(k)];
        PhaseBin b;
        b.freqHz = k * fs_ / fftSize_;
        b.phaseRad = std::arg(lr);
        b.coherence = std::clamp(std::norm(lr) / (ll * rr + 1e-30), 0.0, 1.0);
        b.balance = (ll - rr) / (ll + rr + 1e-30);
        b.levelDbfs = 10.0 * std::log10(0.5 * (ll + rr) * ampScale_ * ampScale_ + 1e-20);
        out.push_back(b);
    }
}

double wrapPi(double rad) noexcept {
    rad = std::fmod(rad + kPi, 2.0 * kPi);
    if (rad < 0) rad += 2.0 * kPi;
    return rad - kPi;
}

bool binVisible(const PhaseBin& b) noexcept { return b.levelDbfs >= -70.0 && b.coherence >= 0.1; }

ScatterPoint scatterPoint(double freqHz, double phaseRad, double fmaxHz, double cx, double cy, double radiusPx) noexcept {
    ScatterPoint p;
    const double f = std::max(freqHz, 1e-9);
    p.radius01 = std::clamp(std::log(f / 20.0) / std::log(fmaxHz / 20.0), 0.0, 1.0);
    p.angle = phaseRad;
    p.x = cx + p.radius01 * radiusPx * std::cos(phaseRad);
    p.y = cy - p.radius01 * radiusPx * std::sin(phaseRad);
    return p;
}

double normalizedCorrelation(const double* a, const double* b, int n) noexcept {
    double sab = 0, saa = 0, sbb = 0;
    for (int i = 0; i < n; ++i) { sab += a[i] * b[i]; saa += a[i] * a[i]; sbb += b[i] * b[i]; }
    const double d = std::sqrt(saa * sbb);
    return d > 1e-30 ? sab / d : 0.0;
}

// ---- Stale / hold / series --------------------------------------------------------------------------
void StaleTracker::update(std::uint64_t generation, std::int64_t sampleIndex, bool valid, double nowMs) noexcept {
    lastValid_ = valid;
    if (!valid) return;
    if (!everValid_ || generation != gen_ || sampleIndex != sample_) {
        lastAdvanceMs_ = nowMs;
        gen_ = generation;
        sample_ = sampleIndex;
    }
    everValid_ = true;
}
bool StaleTracker::stale(double nowMs) const noexcept { return !everValid_ || nowMs - lastAdvanceMs_ > kStaleMs; }
double StaleTracker::ageMs(double nowMs) const noexcept { return everValid_ ? std::max(0.0, nowMs - lastAdvanceMs_) : 1e9; }

void PeakHold::update(double peak, double dtMs) noexcept {
    holdAgeMs += dtMs;
    clipAgeMs += dtMs;
    if (peak >= held || holdAgeMs > 1000.0) { held = peak; holdAgeMs = 0.0; }
    if (peak >= 1.0) clipAgeMs = 0.0;
}

void SeriesHistory::add(double tSeconds, double v) {
    if (!t_.empty() && tSeconds < t_.back()) clear();  // time moved backwards: seek/graph change
    t_.push_back(tSeconds);
    v_.push_back(v);
    while (static_cast<int>(t_.size()) > cap_) { t_.erase(t_.begin()); v_.erase(v_.begin()); }
}

double toDb(double linear, double floorDb) noexcept {
    return linear <= 0 ? floorDb : std::max(floorDb, 20.0 * std::log10(linear));
}

}  // namespace cd::plugin::ui::analysis
