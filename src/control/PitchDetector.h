// PitchDetector (CD-F12 audio source): proposed monophonic normalized-autocorrelation estimator.
// NOT YIN. Research lead only: de Cheveigne & Kawahara, "YIN, a fundamental frequency estimator for
// speech and music", JASA 111(4), 2002 (S12). Thresholds below are PROPOSED; gate G10 (corpus
// calibration of confidence/octave thresholds) remains open.
// Owner module: cd_control. Threading: WORKER thread only, single owner. May allocate in the
// constructor only; submit()/process() do not allocate. Not audio-thread code.
//
// Pipeline: windowed-sinc decimation to 12 kHz (Blackman, cutoff 4.8 kHz) -> 1024-sample Hann
// window, hop 128 -> rho(l) = sum x[n]x[n-l] / sqrt(sum x[n]^2 sum x[n-l]^2 + eps) for l = 6..218 ->
// peaks > 0.8, smallest lag within 0.03 of the max -> parabolic refine. RMS < -60 dBFS rejected.
// Octave-jump rejection unless stable for 3 hops; 2 valid hops acquire; 200 ms invalid holds then
// reverts. Window queue capacity 8, drop-oldest on overload. Results carry sample timestamps and
// stale (>250 ms) results must be rejected by the consumer via isFresh().
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cd::control {

enum class PitchConfidence { Low, High };

struct PitchEstimate {
    bool valid = false;  // false: no pitch (manual/revert)
    double frequencyHz = 0.0;
    PitchConfidence confidence = PitchConfidence::Low;
    double rho = 0.0;                 // normalized autocorrelation at chosen lag (0 if none)
    std::uint64_t timestamp = 0;      // input-sample index at the end of the analysed window
};

struct HopAnalysis {
    bool voiced = false;
    double frequencyHz = 0.0;
    double rho = 0.0;
    bool ambiguous = false;  // other strong peaks not harmonically related to the chosen lag
};

class PitchDetector {
public:
    static constexpr double kAnalysisRate = 12000.0;
    static constexpr int kWindow = 1024;
    static constexpr int kHop = 128;
    static constexpr int kMinLag = 6;
    static constexpr int kMaxLag = 218;
    static constexpr double kPeakThreshold = 0.8;
    static constexpr double kPeakTolerance = 0.03;
    static constexpr double kRmsFloorDb = -60.0;
    static constexpr double kHighRho = 0.9;
    static constexpr double kHoldSeconds = 0.200;
    static constexpr double kStaleSeconds = 0.250;
    static constexpr std::size_t kQueueCapacity = 8;
    static constexpr int kAcquireHops = 2;
    static constexpr int kJumpStableHops = 3;

    explicit PitchDetector(double inputSampleRate);  // allocates
    bool ok() const noexcept { return ok_; }

    // Feed mono input; decimates and enqueues full windows (drops oldest beyond 8). Non-finite
    // samples are treated as zero. No allocation.
    void submit(const float* mono, std::size_t n) noexcept;
    // Analyse queued windows in order; writes up to maxOut estimates. Returns count written.
    std::size_t process(PitchEstimate* out, std::size_t maxOut) noexcept;

    std::size_t pending() const noexcept { return count_; }
    std::uint64_t droppedWindows() const noexcept { return dropped_; }
    void reset() noexcept;

    // Pure per-window analysis (1024 samples at 12 kHz); usable by tests/calibration.
    static HopAnalysis analyzeWindow(const float* window1024) noexcept;
    static bool isFresh(std::uint64_t estimateTimestamp, std::uint64_t nowSample, double sampleRate) noexcept;

private:
    void decimate() noexcept;
    void pushDecimated(float v, double inputTime) noexcept;
    void enqueue(std::uint64_t ts) noexcept;
    PitchEstimate track(const HopAnalysis& a, std::uint64_t ts) noexcept;

    bool ok_ = false;
    double fs_ = 0.0, ratio_ = 0.0;
    int halfIn_ = 0;
    // input history
    std::vector<float> hist_;
    std::size_t histLen_ = 0;
    std::uint64_t histStart_ = 0;  // absolute index of hist_[0]
    double nextT_ = 0.0;           // absolute input time of next decimated sample
    // decimated ring + window queue
    std::vector<float> ring_;      // kWindow
    std::uint64_t decCount_ = 0;
    std::vector<float> queue_;     // kQueueCapacity * kWindow
    std::uint64_t queueTs_[kQueueCapacity] = {};
    std::size_t qHead_ = 0, count_ = 0;
    std::uint64_t dropped_ = 0;
    // tracker state
    enum class State { Idle, Tracking } state_ = State::Idle;
    double pendFreq_ = 0.0;
    int pendCount_ = 0;
    double jumpFreq_ = 0.0;
    int jumpCount_ = 0;
    double f0_ = 0.0;
    std::uint64_t lastValidTs_ = 0;
};

}  // namespace cd::control
