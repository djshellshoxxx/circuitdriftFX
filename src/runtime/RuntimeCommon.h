// CircuitDrift runtime: small preallocated helpers shared by CircuitRuntime, OutputStage and Engine.
// Owner: cd_runtime (runtime agent). Contents: integer delay line, linear ramp, one-pole smoother
// and the declared-latency plan (01-runtime "Oversampling and quality": pad within one phase
// family to the maximum integer latency among 1-16x, compensate fractional delay explicitly).
// Also: StateWriter/StateReader, the fixed-layout binary stream used by engine snapshots (provider
// layouts, see Engine.h) and A/B copy. Every item occupies a multiple of 8 bytes.
// Threading: prepare()/planLatency() allocate and run off the audio thread; every other member
// is noexcept, bounded and allocation-free.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>
#include "dsp/Oversampling.h"

namespace cd::runtime {

class StateWriter;
class StateReader;

// Fixed integer delay (0 = pass-through). Ring storage is sized once in prepare().
class DelayLine {
public:
    void prepare(int delaySamples);           // allocates; delaySamples >= 0
    double process(double x) noexcept {
        if (d_ == 0) return x;
        const double y = buf_[pos_];
        buf_[pos_] = x;
        if (++pos_ == d_) pos_ = 0;
        return y;
    }
    void reset() noexcept;
    int delay() const noexcept { return d_; }
    // Snapshot support (see StateWriter). load() validates delay length and position first; with
    // apply=false it only validates.
    void save(StateWriter& w) const noexcept;
    bool load(StateReader& r, bool apply) noexcept;
private:
    std::vector<double> buf_;
    int d_ = 0, pos_ = 0;
};

// Sequential writer over a caller buffer; a null buffer only counts bytes (layout sizing). A write
// past capacity marks the stream failed and writes nothing further.
class StateWriter {
public:
    StateWriter(void* buf, std::size_t capacity) noexcept : buf_(static_cast<unsigned char*>(buf)), cap_(capacity) {}
    void bytes(const void* p, std::size_t n) noexcept {
        const std::size_t padded = (n + 7) & ~std::size_t(7);
        if (buf_) {
            if (!ok_ || pos_ + padded > cap_) { ok_ = false; pos_ += padded; return; }
            std::memcpy(buf_ + pos_, p, n);
            if (padded > n) std::memset(buf_ + pos_ + n, 0, padded - n);
        }
        pos_ += padded;
    }
    template <class T> void pod(const T& v) noexcept {
        static_assert(std::is_trivially_copyable_v<T>, "snapshot items must be trivially copyable");
        bytes(&v, sizeof(T));
    }
    void doubles(const double* p, std::size_t n) noexcept { bytes(p, n * sizeof(double)); }
    std::size_t size() const noexcept { return pos_; }
    bool ok() const noexcept { return ok_; }
private:
    unsigned char* buf_;
    std::size_t cap_, pos_ = 0;
    bool ok_ = true;
};

class StateReader {
public:
    StateReader(const void* buf, std::size_t size) noexcept : buf_(static_cast<const unsigned char*>(buf)), size_(size) {}
    bool bytes(void* p, std::size_t n) noexcept {
        const std::size_t padded = (n + 7) & ~std::size_t(7);
        if (!ok_ || !buf_ || pos_ + padded > size_) { ok_ = false; return false; }
        if (p) std::memcpy(p, buf_ + pos_, n);
        pos_ += padded;
        return true;
    }
    template <class T> bool pod(T& v) noexcept {
        static_assert(std::is_trivially_copyable_v<T>, "snapshot items must be trivially copyable");
        return bytes(&v, sizeof(T));
    }
    bool doubles(double* p, std::size_t n) noexcept { return bytes(p, n * sizeof(double)); }
    void rewind() noexcept { pos_ = 0; ok_ = true; }
    std::size_t position() const noexcept { return pos_; }
    bool ok() const noexcept { return ok_; }
    bool atEnd() const noexcept { return ok_ && pos_ == size_; }
private:
    const unsigned char* buf_;
    std::size_t size_, pos_ = 0;
    bool ok_ = true;
};

// FNV-1a 64-bit accumulation used for snapshot model identities (not cryptographic).
struct Fnv64 {
    std::uint64_t h = 1469598103934665603ull;
    void bytes(const void* p, std::size_t n) noexcept {
        const unsigned char* c = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) { h ^= c[i]; h *= 1099511628211ull; }
    }
    template <class T> void pod(const T& v) noexcept { bytes(&v, sizeof(T)); }
    void str(const char* s) noexcept { bytes(s, std::strlen(s) + 1); }
};

// Linear ramp that lands exactly on its target after a fixed number of samples.
class LinearRamp {
public:
    void prepare(int rampSamples) noexcept { n_ = rampSamples < 1 ? 1 : rampSamples; }
    void snap(double v) noexcept { value_ = target_ = v; left_ = 0; }
    void setTarget(double t) noexcept {
        if (t == target_) return;
        target_ = t; left_ = n_; step_ = (target_ - value_) / double(n_);
    }
    double next() noexcept {  // returns the value for the current sample, then advances
        if (left_ > 0) { value_ = (--left_ == 0) ? target_ : value_ + step_; }
        return value_;
    }
    double value() const noexcept { return value_; }
    double target() const noexcept { return target_; }
    bool ramping() const noexcept { return left_ > 0; }
private:
    double value_ = 0, target_ = 0, step_ = 0;
    int n_ = 1, left_ = 0;
};

// One-pole exponential smoother with time constant tau; snaps when within 1e-12 relative of target
// so settled parameters become exact (bit-identical comparisons across partitions still hold since
// the update is per sample).
class OnePole {
public:
    void prepare(double fs, double tauSec) noexcept;
    void snap(double v) noexcept { y_ = target_ = v; }
    void setTarget(double t) noexcept { target_ = t; }
    double next() noexcept {
        if (y_ != target_) {
            y_ = target_ + a_ * (y_ - target_);
            const double tol = 1e-12 * (target_ < 0 ? -target_ : target_) + 1e-15;
            if ((y_ - target_ <= tol) && (target_ - y_ <= tol)) y_ = target_;
        }
        return y_;
    }
    double value() const noexcept { return y_; }
    double target() const noexcept { return target_; }
private:
    double a_ = 0, y_ = 0, target_ = 0;
};

// Declared latency of one oversampled stage.
// osLatency: Oversampler::latency() in base samples (FIR exact, IIR nominal DC group delay).
// declared:  integer base-sample latency reported for this stage.
// padHigh:   extra integer delay, in HIGH-rate samples, inserted inside the oversampled domain so
//            that osLatency + padHigh/factor == declared. FIR round-trip latency is always an
//            integer number of high-rate samples, so FIR compensation is exact (residual 0) and
//            lossless (no interpolation). IIR residual is |declared - osLatency - padHigh/F| <=
//            0.5/F base samples at DC only (nominal; IIR phase is frequency dependent).
struct LatencyPlan {
    double osLatency = 0;
    int declared = 0;
    int padHigh = 0;
    double residual = 0;
    double familyMax = 0;   // max latency among 1/2/4/8/16x of this family (when padding)
};

// padToFamilyMax: true for normal modes (pad to max among 1-16x of spec.family at the same pass/
// stop design), false for explicit Zero Latency / unpadded diagnostics. Returns false if any
// required Oversampler design fails. Allocates temporary designs (off-thread only).
bool planLatency(const dsp::Oversampler::Spec& spec, bool padToFamilyMax, LatencyPlan& out);

inline bool isFiniteSample(double x) noexcept { return x - x == 0.0; }

}  // namespace cd::runtime
