#include "control/PitchDetector.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace cd::control {
namespace {
constexpr double kRate = PitchHopTracker::kRate;
constexpr double kPi = std::numbers::pi;
constexpr std::size_t kDsMask = 4095;
constexpr int kChunk = 4096;
constexpr double kRmsGate = 1e-3;  // -60 dBFS
constexpr double kPeakMin = 0.8, kPeakTol = 0.03;
}  // namespace

// ---------------------------------------------------------------- hop tracker
void PitchHopTracker::reset() noexcept {
    acquired_ = false;
    lastF_ = candF_ = octCand_ = 0;
    validRun_ = octCount_ = invalidSamples_ = 0;
}

PitchResult PitchHopTracker::update(bool signalPresent, bool valid, double f, double conf) noexcept {
    PitchResult r;
    if (valid) {
        invalidSamples_ = 0;
        if (acquired_) {
            const double oct = std::fabs(std::log2(f / lastF_));
            const double nearest = std::round(oct);
            const bool octaveJump = nearest >= 1.0 && std::fabs(oct - nearest) < 0.08;
            if (octaveJump) {
                if (octCount_ > 0 && std::fabs(std::log2(f / octCand_)) < 0.04) ++octCount_;
                else { octCand_ = f; octCount_ = 1; }
                if (octCount_ >= 3) { lastF_ = f; octCount_ = 0; }  // stable for 3 hops: accept
            } else {
                octCount_ = 0;
                lastF_ = f;
            }
            r.state = PitchState::Acquired;
            r.frequencyHz = lastF_;
            r.confidence = conf;
            return r;
        }
        octCount_ = 0;
        if (validRun_ > 0 && std::fabs(std::log2(f / candF_)) < 0.05) ++validRun_;
        else validRun_ = 1;
        candF_ = f;
        if (validRun_ >= 2) {
            acquired_ = true;
            lastF_ = f;
            r.state = PitchState::Acquired;
        } else {
            r.state = PitchState::Low;  // one valid hop is not yet a pitch
        }
        r.frequencyHz = f;
        r.confidence = conf;
        return r;
    }
    validRun_ = 0;
    octCount_ = 0;
    if (acquired_) {
        invalidSamples_ += kHopSamples;
        if (invalidSamples_ >= int(0.2 * kRate)) {
            acquired_ = false;
            r.state = PitchState::None;
        } else {
            r.state = PitchState::Low;
            r.held = true;
            r.frequencyHz = lastF_;
        }
        return r;
    }
    r.state = signalPresent ? PitchState::Low : PitchState::None;
    return r;
}

// ---------------------------------------------------------------- detector
void PitchDetector::prepare(double sampleRate, std::size_t ringSamples) {
    prepared_ = false;
    // Validate before any conversion, allocation or power-of-two rounding.
    // In particular, an untrusted SIZE_MAX must not wrap the doubling loop.
    if (!std::isfinite(sampleRate) || sampleRate < kMinSampleRate || sampleRate > kMaxSampleRate ||
        ringSamples > kMaxRingSamples) {
        reset();  // idle-only: clears prior published pitch and advances its generation
        return;
    }
    fs_ = sampleRate;
    std::size_t cap = kMinRingSamples;
    while (cap < ringSamples) cap <<= 1;
    ring_.assign(cap, 0.0);
    mask_ = cap - 1;
    ratio_ = fs_ / kRate;
    fc_ = 5400.0 / fs_;                       // cycles/sample
    halfW_ = 10.0 / (2.0 * fc_);              // ~10 zero crossings per side
    halfTaps_ = int(std::ceil(halfW_));
    chunk_.assign(kChunk, 0.0);
    in_.assign(std::size_t(kChunk + 4 * halfTaps_ + 64), 0.0);
    ds_.assign(kDsMask + 1, 0.0);
    hann_.resize(kWindow);
    for (int n = 0; n < kWindow; ++n) hann_[std::size_t(n)] = 0.5 - 0.5 * std::cos(2.0 * kPi * n / kWindow);
    win_.assign(kWindow, 0.0);
    buf_.assign(kWindow, 0.0);
    cum_.assign(kWindow + 1, 0.0);
    rho_.assign(kMaxLag + 2, 0.0);
    reset();
    prepared_ = true;
}

void PitchDetector::reset() {
    wr_.store(0);
    rd_.store(0);
    droppedSamples_.store(0);
    droppedWindows_.store(0);
    // A saved result must never become fresh again when a session is reset or
    // prepared a second time with the same sample clock and generation.
    workerGeneration_ = generation_.fetch_add(1, std::memory_order_release) + 1;
    seekBoundary_.store(0, std::memory_order_relaxed);
    baseAbs_ = 0;
    resetAnalysis();
    publish(PitchResult{});
}

void PitchDetector::markDiscontinuity() noexcept {
    // The producer publishes its last pre-seek write index before the epoch.
    // Neither ring consumer position nor worker-owned filters are touched here.
    // Release/acquire on the boundary also orders its write index if a worker
    // observes this boundary before it observes the corresponding generation.
    seekBoundary_.store(wr_.load(std::memory_order_relaxed), std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_release);
}

void PitchDetector::resetAnalysis() noexcept {
    baseAbs_ = rd_.load(std::memory_order_relaxed);
    inBase_ = 0;
    nIn_ = 0;
    m0_ = int64_t(std::ceil(double(halfTaps_) / std::max(ratio_, 1e-9)));
    m_ = m0_;
    dsCount_ = 0;
    nextEnd_ = kWindow;
    tracker_.reset();
}

void PitchDetector::push(const double* x, int n) noexcept {
    if (!prepared_ || n <= 0) return;
    const uint64_t w = wr_.load(std::memory_order_relaxed);
    const uint64_t r = rd_.load(std::memory_order_acquire);
    if (w - r + uint64_t(n) > ring_.size()) {  // overload: drop the incoming block, flag a gap
        droppedSamples_.fetch_add(uint64_t(n), std::memory_order_relaxed);
        markDiscontinuity();
        return;
    }
    for (int i = 0; i < n; ++i) ring_[std::size_t(w + uint64_t(i)) & mask_] = x[i];
    wr_.store(w + uint64_t(n), std::memory_order_release);
}

double PitchDetector::kernel(double u) const noexcept {
    const double a = std::fabs(u);
    if (a >= halfW_) return 0.0;
    const double x = 2.0 * fc_ * u;
    const double s = (std::fabs(x) < 1e-12) ? 1.0 : std::sin(kPi * x) / (kPi * x);
    const double ph = kPi * u / halfW_;
    const double win = 0.42 + 0.5 * std::cos(ph) + 0.08 * std::cos(2.0 * ph);
    return 2.0 * fc_ * s * win;
}

void PitchDetector::feed(const double* x, int n) noexcept {
    std::memcpy(in_.data() + nIn_, x, std::size_t(n) * sizeof(double));
    nIn_ += n;
    const int64_t avail = inBase_ + nIn_;  // exclusive
    for (;;) {
        const double t = double(m_) * ratio_;
        const int64_t lo = int64_t(std::ceil(t - halfW_));
        const int64_t hi = int64_t(std::floor(t + halfW_));
        if (hi >= avail) break;
        double acc = 0, wsum = 0;
        for (int64_t k = lo; k <= hi; ++k) {
            const double h = kernel(double(k) - t);
            acc += h * in_[std::size_t(k - inBase_)];
            wsum += h;
        }
        ++m_;
        onDownsampled(wsum != 0.0 ? acc / wsum : 0.0);
    }
    // Trim consumed history.
    const int64_t keepFrom = int64_t(std::floor(double(m_) * ratio_ - halfW_)) - 1;
    const int64_t shift = keepFrom - inBase_;
    if (shift > 0) {
        const int64_t s = std::min<int64_t>(shift, nIn_);
        std::memmove(in_.data(), in_.data() + s, std::size_t(nIn_ - s) * sizeof(double));
        nIn_ -= int(s);
        inBase_ += s;
    }
}

void PitchDetector::onDownsampled(double v) noexcept {
    ds_[std::size_t(dsCount_) & kDsMask] = v;
    ++dsCount_;
    if (dsCount_ != nextEnd_) return;
    nextEnd_ += kHop;
    for (int i = 0; i < kWindow; ++i) win_[std::size_t(i)] = ds_[std::size_t(dsCount_ - kWindow + uint64_t(i)) & kDsMask];
    double f = 0, score = 0, rms = 0;
    const bool valid = estimateWindow(win_.data(), f, score, rms);
    const bool present = rms >= kRmsGate;
    PitchResult r = tracker_.update(present, valid && present, f, score);
    const double tEnd = double(m_ - 1) * ratio_;
    r.timestamp = baseAbs_ + uint64_t(std::max<long long>(0, std::llround(tEnd)));
    publish(r);
}

bool PitchDetector::estimateWindow(const double* x, double& freqHz, double& score, double& rms) noexcept {
    double e = 0;
    for (int n = 0; n < kWindow; ++n) e += x[n] * x[n];
    rms = std::sqrt(e / kWindow);
    freqHz = score = 0;
    if (rms < kRmsGate) return false;
    double* buf = buf_.data();  // Hann-windowed copy (x may alias win_)
    for (int n = 0; n < kWindow; ++n) buf[n] = x[n] * hann_[std::size_t(n)];
    cum_[0] = 0;
    for (int n = 0; n < kWindow; ++n) cum_[std::size_t(n + 1)] = cum_[std::size_t(n)] + buf[n] * buf[n];
    const double eps = 1e-12;
    for (int l = kMinLag - 1; l <= kMaxLag + 1; ++l) {
        double num = 0;
        for (int n = l; n < kWindow; ++n) num += buf[n] * buf[n - l];
        const double sa = cum_[kWindow] - cum_[std::size_t(l)];      // sum x[n]^2, n in [l,N)
        const double sb = cum_[std::size_t(kWindow - l)];            // sum x[n-l]^2
        rho_[std::size_t(l)] = num / std::sqrt(sa * sb + eps);
    }
    double best = 0;
    for (int l = kMinLag; l <= kMaxLag; ++l) {
        const double r = rho_[std::size_t(l)];
        if (r > kPeakMin && r >= rho_[std::size_t(l - 1)] && r > rho_[std::size_t(l + 1)]) best = std::max(best, r);
    }
    if (best <= 0) return false;
    for (int l = kMinLag; l <= kMaxLag; ++l) {
        const double r = rho_[std::size_t(l)];
        if (r > kPeakMin && r >= rho_[std::size_t(l - 1)] && r > rho_[std::size_t(l + 1)] && r >= best - kPeakTol) {
            const double a = rho_[std::size_t(l - 1)], b = r, c = rho_[std::size_t(l + 1)];
            const double den = a - 2.0 * b + c;
            const double delta = (den < -1e-12) ? std::clamp(0.5 * (a - c) / den, -0.5, 0.5) : 0.0;
            freqHz = kRate / (double(l) + delta);
            score = std::min(1.0, r);
            return true;
        }
    }
    return false;
}

void PitchDetector::run() noexcept {
    if (!prepared_) return;
    const uint64_t generation = generation_.load(std::memory_order_acquire);
    if (generation != workerGeneration_) {
        workerGeneration_ = generation;
        // Discard only the pre-seek ring prefix. A worker already consuming an
        // old chunk cannot publish a usable result: latest() checks its epoch.
        const uint64_t boundary = seekBoundary_.load(std::memory_order_acquire);
        const uint64_t read = rd_.load(std::memory_order_relaxed);
        if (read < boundary) rd_.store(boundary, std::memory_order_release);
        resetAnalysis();
    }
    uint64_t rd = rd_.load(std::memory_order_relaxed);
    const uint64_t wr = wr_.load(std::memory_order_acquire);
    // Backlog cap: 8 windows worth of input (drop oldest on overload).
    const uint64_t cap = uint64_t(std::ceil(double(kWindow + (kMaxPendingWindows - 1) * kHop) * ratio_)) + uint64_t(halfTaps_);
    if (wr - rd > cap) {
        const uint64_t skip = wr - rd - cap;
        droppedWindows_.fetch_add(uint64_t(double(skip) / (double(kHop) * ratio_)) + 1, std::memory_order_relaxed);
        rd += skip;
        rd_.store(rd, std::memory_order_release);
        resetAnalysis();
    }
    while (rd < wr) {
        const int n = int(std::min<uint64_t>(uint64_t(kChunk), wr - rd));
        for (int i = 0; i < n; ++i) chunk_[std::size_t(i)] = ring_[std::size_t(rd + uint64_t(i)) & mask_];
        feed(chunk_.data(), n);
        rd += uint64_t(n);
        rd_.store(rd, std::memory_order_release);
    }
}

void PitchDetector::publish(const PitchResult& r) noexcept {
    const uint32_t s = seq_.load(std::memory_order_relaxed);
    seq_.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    pFreq_.store(r.frequencyHz, std::memory_order_relaxed);
    pConf_.store(r.confidence, std::memory_order_relaxed);
    pTs_.store(r.timestamp, std::memory_order_relaxed);
    pGeneration_.store(workerGeneration_, std::memory_order_relaxed);
    pState_.store(uint32_t(r.state), std::memory_order_relaxed);
    pHeld_.store(r.held ? 1u : 0u, std::memory_order_relaxed);
    seq_.store(s + 2, std::memory_order_release);
}

PitchResult PitchDetector::latest() const noexcept {
    PitchResult r;
    for (int tries = 0; tries < 16; ++tries) {
        const uint32_t s1 = seq_.load(std::memory_order_acquire);
        if (s1 & 1u) continue;
        r.frequencyHz = pFreq_.load(std::memory_order_relaxed);
        r.confidence = pConf_.load(std::memory_order_relaxed);
        r.timestamp = pTs_.load(std::memory_order_relaxed);
        r.generation = pGeneration_.load(std::memory_order_relaxed);
        r.state = PitchState(pState_.load(std::memory_order_relaxed));
        r.held = pHeld_.load(std::memory_order_relaxed) != 0;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq_.load(std::memory_order_relaxed) == s1)
            return r.generation == generation_.load(std::memory_order_acquire) ? r : PitchResult{};
    }
    return PitchResult{};  // writer busy: report no pitch rather than block
}

bool PitchDetector::isFresh(const PitchResult& r) const noexcept {
    if (!prepared_ || r.state == PitchState::None ||
        r.generation != generation_.load(std::memory_order_acquire)) return false;
    const uint64_t now = wr_.load(std::memory_order_acquire);
    if (r.timestamp > now) return false;  // a saved result cannot predate the new input clock
    const uint64_t age = now - r.timestamp;
    return double(age) <= 0.25 * fs_;
}

}  // namespace cd::control
