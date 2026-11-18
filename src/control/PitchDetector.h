// cd_control: monophonic audio pitch detector for the Audio key source (CD-F12).
// Proposed normalized-autocorrelation detector (02-musical-controls), NOT YIN and NOT a polyphonic
// analyzer; chords and noise report Low confidence instead of a fabricated pitch.
//
// Pipeline (worker): antialiased fractional downsample to 12 kHz (windowed-sinc, fc 5.4 kHz) ->
// 1024-sample Hann window, hop 128 -> normalized autocorrelation, lags 6..218 (55..2000 Hz) ->
// local peaks > 0.8 -> smallest lag whose score is within 0.03 of the best -> parabolic refinement ->
// reject RMS < -60 dBFS -> octave jumps accepted only after 3 stable hops -> 2 valid hops acquire ->
// 200 ms of invalid hops hold the last pitch (state Low, held) then revert (None).
//
// Threading: prepare()/reset() on a non-audio thread while idle (allocate). push() is the audio-thread
// producer of a preallocated lock-free SPSC ring (noexcept, no allocation, no locks; overload drops the
// incoming block and counts it). run() is the single worker consumer; it may be called from any one
// thread at a time and never blocks. latest() is a bounded-retry lock-free seqlock read (any thread).
// If the worker falls behind by more than 8 windows the oldest input is dropped (counted).
// Timestamps are in accepted-input-sample units (the stream that push() fed); results older than 250 ms
// relative to pushedSamples() are stale and rejected by isFresh().
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "control/ControlTypes.h"

namespace cd::control {

struct PitchResult {
    double frequencyHz = 0.0;
    double confidence = 0.0;      // normalized autocorrelation score of the chosen peak
    uint64_t timestamp = 0;       // accepted input sample index at the window end
    PitchState state = PitchState::None;
    bool held = false;            // frequency retained during the 200 ms invalid hold
};

// Per-hop acquisition/hold state machine (exposed for deterministic testing).
class PitchHopTracker {
public:
    static constexpr int kHopSamples = 128;           // at 12 kHz
    static constexpr double kRate = 12000.0;
    void reset() noexcept;
    // signalPresent: RMS above gate; valid: a peak > 0.8 was found with frequency f / score conf.
    PitchResult update(bool signalPresent, bool valid, double f, double conf) noexcept;

private:
    bool acquired_ = false;
    double lastF_ = 0, candF_ = 0, octCand_ = 0;
    int validRun_ = 0, octCount_ = 0, invalidSamples_ = 0;
};

class PitchDetector {
public:
    static constexpr int kWindow = 1024;
    static constexpr int kHop = 128;
    static constexpr int kMinLag = 6, kMaxLag = 218;
    static constexpr int kMaxPendingWindows = 8;

    void prepare(double sampleRate, std::size_t ringSamples = 1u << 16);  // allocates
    void reset();                                                          // idle only

    void push(const double* mono, int n) noexcept;  // audio thread
    void run() noexcept;                             // worker thread

    PitchResult latest() const noexcept;
    bool isFresh(const PitchResult& r) const noexcept;
    uint64_t pushedSamples() const noexcept { return wr_.load(std::memory_order_acquire); }
    uint64_t droppedSamples() const noexcept { return droppedSamples_.load(std::memory_order_relaxed); }
    uint64_t droppedWindows() const noexcept { return droppedWindows_.load(std::memory_order_relaxed); }
    bool prepared() const noexcept { return prepared_; }

    // Autocorrelation estimate on one 1024-sample window at 12 kHz (worker/test helper, no allocation
    // beyond prepare()). Returns false when no peak > 0.8; rms is the raw window RMS (linear).
    bool estimateWindow(const double* x1024, double& freqHz, double& score, double& rms) noexcept;

private:
    void resetAnalysis() noexcept;
    void feed(const double* x, int n) noexcept;
    void onDownsampled(double v) noexcept;
    void publish(const PitchResult& r) noexcept;
    double kernel(double u) const noexcept;

    bool prepared_ = false;
    double fs_ = 48000.0, ratio_ = 4.0, halfW_ = 0, fc_ = 0;
    int halfTaps_ = 0;

    // SPSC ring.
    std::vector<double> ring_;
    std::size_t mask_ = 0;
    alignas(64) std::atomic<uint64_t> wr_{0};
    alignas(64) std::atomic<uint64_t> rd_{0};
    std::atomic<uint64_t> droppedSamples_{0}, droppedWindows_{0}, gaps_{0};
    uint64_t seenGaps_ = 0;

    // Worker analysis state.
    std::vector<double> chunk_, in_;
    uint64_t baseAbs_ = 0;      // accepted-sample index at analysis reset
    int64_t inBase_ = 0;        // index (since reset) of in_[0]
    int nIn_ = 0;
    int64_t m_ = 0;             // next output index
    int64_t m0_ = 0;
    uint64_t dsCount_ = 0;
    uint64_t nextEnd_ = 0;
    std::vector<double> ds_;
    std::vector<double> hann_, win_, buf_, cum_, rho_;
    PitchHopTracker tracker_;

    // Published result (seqlock).
    mutable std::atomic<uint32_t> seq_{0};
    std::atomic<double> pFreq_{0}, pConf_{0};
    std::atomic<uint64_t> pTs_{0};
    std::atomic<uint32_t> pState_{0}, pHeld_{0};
};

}  // namespace cd::control
