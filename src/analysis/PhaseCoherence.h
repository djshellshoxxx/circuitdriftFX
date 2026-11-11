// CircuitDrift analysis: stereo phase/coherence analysis (UX-06).
// Owner: cd_analysis. Worker thread. Own iterative radix-2 FFT (no third-party code). STFT with
// Hann window, hop size, per-bin cross spectrum S_LR = E[X_L conj(X_R)] and powers S_LL, S_RR,
// phase = arg(S_LR) (positive when R is a delayed copy of L), coherence = |S_LR|^2/(S_LL*S_RR+eps)
// clamped to [0,1], balance = (S_LL-S_RR)/(S_LL+S_RR+eps). Averaging is either Welch (plain mean
// of all frames since reset; tauSeconds == 0) or exponential with time constant tauSeconds
// (spec: 0.25 s). A single frame is trivially coherent: results are marked `settled` only after
// minFrames. All storage is allocated in the constructor; processBlock() does not allocate.
// DC offset and fullband correlation are reported separately by StereoDcCorrelation and are never
// inferred from phase bins. Mono input (right == nullptr) yields "Not applicable".
#pragma once
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cd::analysis {

class Fft {
public:
    explicit Fft(std::size_t n);  // n power of two >= 2, throws std::invalid_argument otherwise
    std::size_t size() const noexcept { return n_; }
    void forward(std::complex<double>* data) const noexcept;  // in place, unnormalised
private:
    std::size_t n_;
    std::vector<std::size_t> rev_;
    std::vector<std::complex<double>> tw_;
};

struct PhaseConfig {
    std::size_t fftSize = 2048;
    std::size_t hop = 512;
    double sampleRate = 48000.0;  // analysis rate (<=48 kHz after antialiasing by the caller)
    double tauSeconds = 0.25;     // 0 => Welch plain average
    double hideBelowDbfs = -70.0;
    double hideBelowCoherence = 0.1;
    double epsilon = 1e-20;
    std::size_t minFrames = 4;  // frames averaged before results are `settled`
};

struct PhaseBin {
    double freqHz = 0;
    double phaseRad = 0;      // (-pi, pi]
    double coherence = 0;     // gamma^2 in [0,1]
    double magnitudeDbfs = -300;
    double balance = 0;       // (S_LL - S_RR)/(S_LL + S_RR + eps)
    bool visible = false;     // above -70 dBFS and coherence >= 0.1
};

struct PhaseSnapshot {
    bool applicable = false;  // false for mono: show "Not applicable"
    bool settled = false;
    std::size_t frames = 0;
};

class StereoPhaseAnalyzer {
public:
    explicit StereoPhaseAnalyzer(const PhaseConfig& cfg);  // validates; throws std::invalid_argument
    void reset() noexcept;
    // Feed one block. right == nullptr means mono source (not applicable). Non-finite samples are
    // treated as 0. Mixed mono/stereo calls reset the averages. noexcept, no allocation.
    void process(const float* left, const float* right, std::size_t n) noexcept;
    std::size_t binCount() const noexcept { return cfg_.fftSize / 2; }  // bins 1..N/2 (DC excluded)
    // Fills out[0..binCount()-1]; returns snapshot status. For mono, out is left zeroed.
    PhaseSnapshot snapshot(PhaseBin* out) const noexcept;
    const PhaseConfig& config() const noexcept { return cfg_; }
    // Polar plot radius: log(f/20)/log(fmax/20) clamped to [0,1].
    static double polarRadius(double freqHz, double fmaxHz) noexcept;

private:
    void runFrame() noexcept;
    PhaseConfig cfg_;
    Fft fft_;
    std::vector<double> window_;
    std::vector<float> ringL_, ringR_;  // last fftSize samples
    std::size_t fill_ = 0, sinceHop_ = 0, writePos_ = 0;
    std::vector<std::complex<double>> bufL_, bufR_, slr_;
    std::vector<double> sll_, srr_;
    std::size_t frames_ = 0;
    double windowSum_ = 0;
    bool stereo_ = false, modeKnown_ = false;
};

// Separate readouts: 500 ms boxcar mean of pre-protection L/R (DC difference) and fullband
// normalised correlation over the same window. Window ring allocated in the constructor.
struct DcCorrelationReadout {
    bool applicable = false;
    double meanL = 0, meanR = 0, dcDifference = 0;  // same units as the input (volts)
    double correlation = 0;                         // [-1,1], 0 if either channel is silent
};

class StereoDcCorrelation {
public:
    explicit StereoDcCorrelation(double sampleRate, double windowSeconds = 0.5);
    void reset() noexcept;
    void process(const float* left, const float* right, std::size_t n) noexcept;  // right==nullptr: mono
    DcCorrelationReadout readout() const noexcept;

private:
    std::vector<float> l_, r_;
    std::size_t pos_ = 0, count_ = 0;
    double sl_ = 0, sr_ = 0, sll_ = 0, srr_ = 0, slr_ = 0;
    std::size_t sinceRecompute_ = 0;
    bool stereo_ = false;
    void recompute() noexcept;
};

}  // namespace cd::analysis
