// CircuitDrift analysis: stereo phase/coherence implementation. See PhaseCoherence.h.
#include "analysis/PhaseCoherence.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace cd::analysis {

namespace {
constexpr double kPi = 3.14159265358979323846;
inline float sanitize(float x) noexcept { return std::isfinite(x) ? x : 0.0f; }
bool isPow2(std::size_t n) { return n >= 2 && (n & (n - 1)) == 0; }
}  // namespace

Fft::Fft(std::size_t n) : n_(n) {
    if (!isPow2(n) || n > (std::size_t(1) << 20)) throw std::invalid_argument("Fft size must be a power of two");
    rev_.resize(n);
    std::size_t bits = 0;
    while ((std::size_t(1) << bits) < n) ++bits;
    for (std::size_t i = 0; i < n; ++i) {
        std::size_t r = 0;
        for (std::size_t b = 0; b < bits; ++b)
            if (i & (std::size_t(1) << b)) r |= std::size_t(1) << (bits - 1 - b);
        rev_[i] = r;
    }
    tw_.resize(n / 2);
    for (std::size_t k = 0; k < n / 2; ++k)
        tw_[k] = std::polar(1.0, -2.0 * kPi * double(k) / double(n));
}

void Fft::forward(std::complex<double>* d) const noexcept {
    for (std::size_t i = 0; i < n_; ++i)
        if (rev_[i] > i) std::swap(d[i], d[rev_[i]]);
    for (std::size_t len = 2; len <= n_; len <<= 1) {
        const std::size_t half = len / 2, step = n_ / len;
        for (std::size_t base = 0; base < n_; base += len) {
            for (std::size_t j = 0; j < half; ++j) {
                const std::complex<double> t = tw_[j * step] * d[base + j + half];
                d[base + j + half] = d[base + j] - t;
                d[base + j] += t;
            }
        }
    }
}

StereoPhaseAnalyzer::StereoPhaseAnalyzer(const PhaseConfig& cfg) : cfg_(cfg), fft_(cfg.fftSize) {
    if (cfg.hop == 0 || cfg.hop > cfg.fftSize || !(cfg.sampleRate > 0.0) || !(cfg.sampleRate <= 192000.0) ||
        !std::isfinite(cfg.tauSeconds) || cfg.tauSeconds < 0.0 || !(cfg.epsilon > 0.0) || cfg.minFrames == 0)
        throw std::invalid_argument("invalid PhaseConfig");
    const std::size_t n = cfg.fftSize;
    window_.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        window_[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * double(i) / double(n));  // periodic Hann
        windowSum_ += window_[i];
    }
    ringL_.assign(n, 0.0f);
    ringR_.assign(n, 0.0f);
    bufL_.resize(n);
    bufR_.resize(n);
    slr_.assign(n / 2 + 1, {});
    sll_.assign(n / 2 + 1, 0.0);
    srr_.assign(n / 2 + 1, 0.0);
}

void StereoPhaseAnalyzer::reset() noexcept {
    std::fill(ringL_.begin(), ringL_.end(), 0.0f);
    std::fill(ringR_.begin(), ringR_.end(), 0.0f);
    std::fill(slr_.begin(), slr_.end(), std::complex<double>{});
    std::fill(sll_.begin(), sll_.end(), 0.0);
    std::fill(srr_.begin(), srr_.end(), 0.0);
    fill_ = sinceHop_ = writePos_ = 0;
    frames_ = 0;
    modeKnown_ = false;
    stereo_ = false;
}

void StereoPhaseAnalyzer::process(const float* left, const float* right, std::size_t n) noexcept {
    if (!left || n == 0) return;
    const bool st = right != nullptr;
    if (modeKnown_ && st != stereo_) reset();
    modeKnown_ = true;
    stereo_ = st;
    if (!st) return;  // mono: not applicable, nothing to analyse
    const std::size_t N = cfg_.fftSize;
    for (std::size_t i = 0; i < n; ++i) {
        ringL_[writePos_] = sanitize(left[i]);
        ringR_[writePos_] = sanitize(right[i]);
        writePos_ = (writePos_ + 1) % N;
        if (fill_ < N) ++fill_;
        if (++sinceHop_ >= cfg_.hop && fill_ == N) {
            sinceHop_ = 0;
            runFrame();
        }
    }
}

void StereoPhaseAnalyzer::runFrame() noexcept {
    const std::size_t N = cfg_.fftSize;
    for (std::size_t i = 0; i < N; ++i) {
        const std::size_t idx = (writePos_ + i) % N;  // writePos_ is the oldest sample
        bufL_[i] = {double(ringL_[idx]) * window_[i], 0.0};
        bufR_[i] = {double(ringR_[idx]) * window_[i], 0.0};
    }
    fft_.forward(bufL_.data());
    fft_.forward(bufR_.data());
    double alpha;
    if (frames_ == 0) alpha = 1.0;
    else if (cfg_.tauSeconds == 0.0) alpha = 1.0 / double(frames_ + 1);
    else alpha = 1.0 - std::exp(-double(cfg_.hop) / (cfg_.sampleRate * cfg_.tauSeconds));
    for (std::size_t k = 1; k <= N / 2; ++k) {
        const std::complex<double> xl = bufL_[k], xr = bufR_[k];
        slr_[k] += alpha * (xl * std::conj(xr) - slr_[k]);
        sll_[k] += alpha * (std::norm(xl) - sll_[k]);
        srr_[k] += alpha * (std::norm(xr) - srr_[k]);
    }
    ++frames_;
}

PhaseSnapshot StereoPhaseAnalyzer::snapshot(PhaseBin* out) const noexcept {
    PhaseSnapshot s;
    const std::size_t N = cfg_.fftSize;
    if (!out) return s;
    for (std::size_t k = 1; k <= N / 2; ++k) out[k - 1] = PhaseBin{};
    if (!(modeKnown_ && stereo_)) return s;  // mono / nothing yet: N/A
    s.applicable = true;
    s.frames = frames_;
    s.settled = frames_ >= cfg_.minFrames;
    if (frames_ == 0) return s;
    for (std::size_t k = 1; k <= N / 2; ++k) {
        PhaseBin& b = out[k - 1];
        const double ll = sll_[k], rr = srr_[k];
        b.freqHz = double(k) * cfg_.sampleRate / double(N);
        b.phaseRad = std::arg(slr_[k]);
        b.coherence = std::clamp(std::norm(slr_[k]) / (ll * rr + cfg_.epsilon), 0.0, 1.0);
        b.balance = (ll - rr) / (ll + rr + cfg_.epsilon);
        const double amp = std::sqrt(0.5 * (ll + rr)) * 2.0 / windowSum_;
        b.magnitudeDbfs = 20.0 * std::log10(std::max(amp, 1e-15));
        b.visible = s.settled && b.magnitudeDbfs >= cfg_.hideBelowDbfs && b.coherence >= cfg_.hideBelowCoherence;
    }
    return s;
}

double StereoPhaseAnalyzer::polarRadius(double f, double fmax) noexcept {
    if (!(f > 0.0) || !(fmax > 20.0) || !std::isfinite(f) || !std::isfinite(fmax)) return 0.0;
    return std::clamp(std::log(f / 20.0) / std::log(fmax / 20.0), 0.0, 1.0);
}

// ---------------- DC / correlation ----------------

StereoDcCorrelation::StereoDcCorrelation(double fs, double windowSeconds) {
    if (!(fs > 0.0) || !(fs <= 192000.0) || !(windowSeconds > 0.0) || windowSeconds > 10.0)
        throw std::invalid_argument("invalid StereoDcCorrelation args");
    const std::size_t n = std::max<std::size_t>(1, std::size_t(fs * windowSeconds + 0.5));
    l_.assign(n, 0.0f);
    r_.assign(n, 0.0f);
}

void StereoDcCorrelation::reset() noexcept {
    std::fill(l_.begin(), l_.end(), 0.0f);
    std::fill(r_.begin(), r_.end(), 0.0f);
    pos_ = count_ = 0;
    sl_ = sr_ = sll_ = srr_ = slr_ = 0;
    sinceRecompute_ = 0;
    stereo_ = false;
}

void StereoDcCorrelation::recompute() noexcept {
    sl_ = sr_ = sll_ = srr_ = slr_ = 0;
    for (std::size_t i = 0; i < count_; ++i) {
        const double a = l_[i], b = r_[i];
        sl_ += a; sr_ += b; sll_ += a * a; srr_ += b * b; slr_ += a * b;
    }
    sinceRecompute_ = 0;
}

void StereoDcCorrelation::process(const float* left, const float* right, std::size_t n) noexcept {
    if (!left || n == 0) return;
    if (!right) { if (stereo_) reset(); return; }
    stereo_ = true;
    const std::size_t cap = l_.size();
    for (std::size_t i = 0; i < n; ++i) {
        const float a = sanitize(left[i]), b = sanitize(right[i]);
        if (count_ == cap) {
            const double oa = l_[pos_], ob = r_[pos_];
            sl_ -= oa; sr_ -= ob; sll_ -= oa * oa; srr_ -= ob * ob; slr_ -= oa * ob;
        } else {
            ++count_;
        }
        l_[pos_] = a;
        r_[pos_] = b;
        sl_ += a; sr_ += b; sll_ += double(a) * a; srr_ += double(b) * b; slr_ += double(a) * b;
        pos_ = (pos_ + 1) % cap;
        if (++sinceRecompute_ >= cap * 4) recompute();  // bound floating-point drift
    }
}

DcCorrelationReadout StereoDcCorrelation::readout() const noexcept {
    DcCorrelationReadout r;
    if (!stereo_ || count_ == 0) return r;
    r.applicable = true;
    const double n = double(count_);
    r.meanL = sl_ / n;
    r.meanR = sr_ / n;
    r.dcDifference = r.meanL - r.meanR;
    const double vl = sll_ / n - r.meanL * r.meanL, vr = srr_ / n - r.meanR * r.meanR;
    const double cov = slr_ / n - r.meanL * r.meanR;
    // Variance floor relative to power guards against cancellation noise on silent channels.
    const double fl = 1e-12 * (sll_ / n + 1e-30), fr = 1e-12 * (srr_ / n + 1e-30);
    if (vl > fl && vr > fr) r.correlation = std::clamp(cov / std::sqrt(vl * vr), -1.0, 1.0);
    return r;
}

}  // namespace cd::analysis
