#include "control/PitchDetector.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace cd::control {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kCutoff = 0.40;   // of the 12 kHz output rate (4.8 kHz)
constexpr double kHalfOut = 24.0;  // kernel half-width in output samples
constexpr std::size_t kChunk = 4096;

double cents(double a, double b) { return 1200.0 * std::log2(a / b); }

double kernel(double u) noexcept {  // u in output-sample units
    const double au = std::fabs(u);
    if (au >= kHalfOut) return 0.0;
    const double x = 2.0 * kCutoff * u;
    const double s = std::fabs(x) < 1e-12 ? 1.0 : std::sin(kPi * x) / (kPi * x);
    const double v = u / kHalfOut;
    const double w = 0.42 + 0.5 * std::cos(kPi * v) + 0.08 * std::cos(2.0 * kPi * v);
    return 2.0 * kCutoff * s * w;
}
}  // namespace

PitchDetector::PitchDetector(double fs) {
    if (!std::isfinite(fs) || fs < 16000.0 || fs > 384000.0) return;
    fs_ = fs;
    ratio_ = fs / kAnalysisRate;
    halfIn_ = static_cast<int>(std::ceil(kHalfOut * ratio_));
    hist_.assign(kChunk + 2 * static_cast<std::size_t>(halfIn_) + 16, 0.0f);
    ring_.assign(kWindow, 0.0f);
    queue_.assign(kQueueCapacity * kWindow, 0.0f);
    ok_ = true;
    reset();
}

void PitchDetector::reset() noexcept {
    histLen_ = 0;
    histStart_ = 0;
    nextT_ = 0.0;
    decCount_ = 0;
    qHead_ = count_ = 0;
    dropped_ = 0;
    state_ = State::Idle;
    pendCount_ = jumpCount_ = 0;
    f0_ = 0.0;
    lastValidTs_ = 0;
    if (ok_) std::fill(ring_.begin(), ring_.end(), 0.0f);
}

void PitchDetector::submit(const float* mono, std::size_t n) noexcept {
    if (!ok_ || mono == nullptr) return;
    while (n > 0) {
        const std::size_t take = std::min(n, kChunk);
        for (std::size_t i = 0; i < take; ++i) {
            const float v = mono[i];
            hist_[histLen_ + i] = std::isfinite(v) ? v : 0.0f;
        }
        histLen_ += take;
        mono += take;
        n -= take;
        decimate();
    }
}

void PitchDetector::decimate() noexcept {
    const std::int64_t end = static_cast<std::int64_t>(histStart_ + histLen_);  // one past last
    for (;;) {
        const double t = nextT_;
        const std::int64_t hi = static_cast<std::int64_t>(std::floor(t + halfIn_));
        if (hi >= end) break;
        const std::int64_t lo = static_cast<std::int64_t>(std::ceil(t - halfIn_));
        double acc = 0.0, wsum = 0.0;
        for (std::int64_t m = lo; m <= hi; ++m) {
            const double h = kernel((static_cast<double>(m) - t) / ratio_);
            wsum += h;
            if (m >= static_cast<std::int64_t>(histStart_) && m < end)
                acc += h * hist_[static_cast<std::size_t>(m - static_cast<std::int64_t>(histStart_))];
        }
        pushDecimated(wsum > 0.0 ? static_cast<float>(acc / wsum) : 0.0f, t);
        nextT_ += ratio_;
    }
    // Trim consumed history.
    std::int64_t keep = static_cast<std::int64_t>(std::ceil(nextT_ - halfIn_));
    keep = std::max<std::int64_t>(keep, static_cast<std::int64_t>(histStart_));
    keep = std::min<std::int64_t>(keep, end);
    const std::size_t drop = static_cast<std::size_t>(keep - static_cast<std::int64_t>(histStart_));
    if (drop > 0) {
        std::memmove(hist_.data(), hist_.data() + drop, (histLen_ - drop) * sizeof(float));
        histLen_ -= drop;
        histStart_ += drop;
    }
}

void PitchDetector::pushDecimated(float v, double t) noexcept {
    ring_[static_cast<std::size_t>(decCount_ % kWindow)] = v;
    ++decCount_;
    if (decCount_ >= static_cast<std::uint64_t>(kWindow) && (decCount_ - kWindow) % kHop == 0)
        enqueue(static_cast<std::uint64_t>(std::llround(t)));
}

void PitchDetector::enqueue(std::uint64_t ts) noexcept {
    if (count_ == kQueueCapacity) {  // overload: drop the oldest window
        qHead_ = (qHead_ + 1) % kQueueCapacity;
        --count_;
        ++dropped_;
    }
    const std::size_t slot = (qHead_ + count_) % kQueueCapacity;
    float* dst = queue_.data() + slot * kWindow;
    const std::size_t start = static_cast<std::size_t>(decCount_ % kWindow);  // oldest sample
    for (std::size_t i = 0; i < static_cast<std::size_t>(kWindow); ++i) dst[i] = ring_[(start + i) % kWindow];
    queueTs_[slot] = ts;
    ++count_;
}

HopAnalysis PitchDetector::analyzeWindow(const float* w) noexcept {
    HopAnalysis r;
    if (w == nullptr) return r;
    constexpr int N = kWindow;
    double sq = 0.0;
    for (int i = 0; i < N; ++i) sq += static_cast<double>(w[i]) * w[i];
    const double rms = std::sqrt(sq / N);
    if (!(rms >= std::pow(10.0, kRmsFloorDb / 20.0))) return r;

    double x[N];
    double cs[N + 1];
    cs[0] = 0.0;
    for (int i = 0; i < N; ++i) {
        const double hann = 0.5 * (1.0 - std::cos(2.0 * kPi * i / N));
        x[i] = hann * w[i];
        cs[i + 1] = cs[i] + x[i] * x[i];
    }
    double rho[kMaxLag + 2] = {};
    for (int l = kMinLag - 1; l <= kMaxLag + 1; ++l) {
        double num = 0.0;
        for (int n = l; n < N; ++n) num += x[n] * x[n - l];
        const double e1 = cs[N] - cs[l];
        const double e2 = cs[N - l];
        rho[l] = num / std::sqrt(e1 * e2 + 1e-9);
    }
    int peaks[kMaxLag + 1];
    int np = 0;
    double mx = 0.0;
    for (int l = kMinLag; l <= kMaxLag; ++l)
        if (rho[l] > kPeakThreshold && rho[l] >= rho[l - 1] && rho[l] > rho[l + 1]) {
            peaks[np++] = l;
            mx = std::max(mx, rho[l]);
        }
    if (np == 0) return r;
    int lag = -1;
    for (int i = 0; i < np; ++i)
        if (rho[peaks[i]] >= mx - kPeakTolerance) { lag = peaks[i]; break; }
    const double y1 = rho[lag - 1], y2 = rho[lag], y3 = rho[lag + 1];
    const double den = y1 - 2.0 * y2 + y3;
    double off = den < -1e-12 ? 0.5 * (y1 - y3) / den : 0.0;
    off = std::max(-1.0, std::min(1.0, off));
    const double ref = lag + off;
    r.voiced = true;
    r.frequencyHz = kAnalysisRate / ref;
    r.rho = y2;
    for (int i = 0; i < np; ++i) {
        const int m = peaks[i];
        const double k = std::round(static_cast<double>(m) / lag);
        if (k < 1.0 || std::fabs(m - k * lag) > 0.04 * m + 1.0) { r.ambiguous = true; break; }
    }
    return r;
}

PitchEstimate PitchDetector::track(const HopAnalysis& a, std::uint64_t ts) noexcept {
    PitchEstimate e;
    e.timestamp = ts;
    const std::uint64_t holdSamples = static_cast<std::uint64_t>(kHoldSeconds * fs_);
    if (state_ == State::Idle) {
        if (a.voiced) {
            if (pendCount_ > 0 && std::fabs(cents(a.frequencyHz, pendFreq_)) <= 50.0) ++pendCount_;
            else pendCount_ = 1;
            pendFreq_ = a.frequencyHz;
            if (pendCount_ >= kAcquireHops) {
                state_ = State::Tracking;
                f0_ = a.frequencyHz;
                lastValidTs_ = ts;
                jumpCount_ = 0;
            }
        } else {
            pendCount_ = 0;
        }
        if (state_ == State::Idle) return e;  // no valid pitch yet
    } else if (a.voiced) {
        if (std::fabs(cents(a.frequencyHz, f0_)) <= 600.0) {
            f0_ = a.frequencyHz;
            lastValidTs_ = ts;
            jumpCount_ = 0;
        } else {  // octave-class jump: needs 3 mutually stable hops
            if (jumpCount_ > 0 && std::fabs(cents(a.frequencyHz, jumpFreq_)) <= 50.0) ++jumpCount_;
            else jumpCount_ = 1;
            jumpFreq_ = a.frequencyHz;
            if (jumpCount_ >= kJumpStableHops) {
                f0_ = a.frequencyHz;
                lastValidTs_ = ts;
                jumpCount_ = 0;
            }
        }
    } else {
        jumpCount_ = 0;
        if (ts - lastValidTs_ > holdSamples) {  // hold expired: revert to manual
            state_ = State::Idle;
            pendCount_ = 0;
            return e;
        }
    }
    e.valid = true;
    e.frequencyHz = f0_;
    e.rho = a.voiced ? a.rho : 0.0;
    const bool fresh = (ts == lastValidTs_) && a.voiced && jumpCount_ == 0;
    e.confidence = (fresh && !a.ambiguous && a.rho >= kHighRho) ? PitchConfidence::High : PitchConfidence::Low;
    return e;
}

std::size_t PitchDetector::process(PitchEstimate* out, std::size_t maxOut) noexcept {
    std::size_t n = 0;
    while (ok_ && count_ > 0 && n < maxOut && out != nullptr) {
        const HopAnalysis a = analyzeWindow(queue_.data() + qHead_ * kWindow);
        out[n++] = track(a, queueTs_[qHead_]);
        qHead_ = (qHead_ + 1) % kQueueCapacity;
        --count_;
    }
    return n;
}

bool PitchDetector::isFresh(std::uint64_t ts, std::uint64_t now, double fs) noexcept {
    if (!(fs > 0.0) || ts > now) return ts == now;
    return static_cast<double>(now - ts) <= kStaleSeconds * fs;
}

}  // namespace cd::control
