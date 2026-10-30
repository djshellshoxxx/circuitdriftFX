// CircuitDrift dsp: oversampling (CD-F21 / T21).
// Halfband2x: Kaiser-windowed linear-phase halfband FIR (normal production family).
// HalfbandIir2x: polyphase allpass elliptic halfband (optional low-latency family, nonlinear phase).
// Oversampler: 2^k cascade (1x..64x; 32x/64x intended only for the offline T21 reference).
// Threading: design/prepare allocate and run off the audio thread; process/reset are noexcept,
// allocation-free, double precision. Coefficients are frozen at prepare for a given Spec.
#pragma once
#include <cstdint>
#include <vector>

namespace cd::dsp {

// Linear-phase halfband FIR, length 2c+1 with c odd. Designed for DC gain 1.
class Halfband2x {
public:
    // passEdge: normalized to the high (2x) rate, 0 < passEdge < 0.25; stop edge = 0.5 - passEdge.
    // stopbandDb: Kaiser design attenuation target. Returns false (unprepared) on invalid input.
    bool design(double passEdge, double stopbandDb);
    int center() const noexcept { return c_; }              // group delay in high-rate samples
    const std::vector<double>& taps() const noexcept { return h_; }
    double magnitude(double fNorm) const noexcept;          // |H| at f/Fs_high, from coefficients
    // Up: one base sample -> two high-rate samples (gain 2 applied). Down: two -> one.
    void up(double x, double* out2) noexcept;
    double down(const double* in2) noexcept;
    void reset() noexcept;
private:
    double dotEven(const std::vector<double>& line, int pos) const noexcept;
    std::vector<double> h_, even_;                          // even_ = h[0], h[2], ..., h[2c]
    std::vector<double> upLine_, dnEven_, dnOdd_;          // doubled ring buffers (len 2*E)
    int c_ = 0, E_ = 0, upPos_ = 0, dnPos_ = 0, oddPos_ = 0, oddLen_ = 0;
};

// Two-path polyphase allpass halfband (elliptic). Power complementary: passband deviation ~ stop^2.
class HalfbandIir2x {
public:
    // Chooses the smallest coefficient count (<= kMaxCoefs) meeting stopbandDb at stop edge
    // 0.5 - passEdge (normalized to the high rate). Returns false if unreachable.
    bool design(double passEdge, double stopbandDb);
    static constexpr int kMaxCoefs = 32;
    int coefCount() const noexcept { return n_; }
    double magnitude(double fNorm) const noexcept;
    double groupDelayDc() const noexcept;                   // high-rate samples, nominal only
    void up(double x, double* out2) noexcept;
    double down(const double* in2) noexcept;
    void reset() noexcept;
private:
    struct Ap { double a = 0, x1 = 0, y1 = 0; };
    double path(Ap* s, int count, double x) noexcept;
    double coef_[kMaxCoefs] = {};
    Ap upA_[kMaxCoefs / 2], upB_[kMaxCoefs / 2], dnA_[kMaxCoefs / 2], dnB_[kMaxCoefs / 2];
    int n_ = 0, nA_ = 0, nB_ = 0;
};

enum class OsFamily : uint8_t { Fir, Iir };

class Oversampler {
public:
    struct Spec {
        int factor = 4;                 // 1,2,4,8,16 (product); 32,64 offline reference only
        OsFamily family = OsFamily::Fir;
        double passFraction = 0.45;     // protected band edge as a fraction of the base rate
        double stopbandDb = 100.0;      // per-stage target (01-runtime: >=100 dB)
    };
    bool prepare(const Spec& s, int maxBlock);
    int factor() const noexcept { return factor_; }
    int stages() const noexcept { return stages_; }
    // Round-trip (up+down) latency in base-rate samples. FIR: exact (may be fractional);
    // IIR: nominal DC group delay only (phase is frequency dependent).
    double latency() const noexcept { return latency_; }
    const Halfband2x& firStage(int s) const noexcept { return fir_[s]; }
    const HalfbandIir2x& iirStage(int s) const noexcept { return iir_[s]; }
    // n <= maxBlock. up writes n*factor samples; down reads n*factor and writes n.
    void up(const double* in, double* out, int n) noexcept;
    void down(const double* in, double* out, int n) noexcept;
    void reset() noexcept;
private:
    static constexpr int kMaxStages = 6;
    Halfband2x fir_[kMaxStages];
    HalfbandIir2x iir_[kMaxStages];
    std::vector<double> bufA_, bufB_;
    OsFamily family_ = OsFamily::Fir;
    int factor_ = 0, stages_ = 0, maxBlock_ = 0;
    double latency_ = 0;
};

}  // namespace cd::dsp
