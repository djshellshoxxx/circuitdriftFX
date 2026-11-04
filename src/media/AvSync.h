// CircuitDrift media: audio/video timeline and sync model (P13, T03). Owner: cd_media.
// All timestamps are positions on the AUDIO SAMPLE CLOCK (int64 sample index), never wall time.
//  - tickSample(): output video tick n sits at round(n*rate/fps) samples (30 FPS video, 20 FPS GIF
//    = 50 ms / 5 cs per frame).
//  - AvSyncModel: running statistics of skew = (tick time - content time) in ms and the drift
//    (least-squares slope of skew over the timeline, scaled to the observed span). Worker thread.
//  - ClockMapper: UI-side wall-clock -> sample-clock estimator (least squares over the last 64
//    observations of the audio tap's published sample position); fixed storage, no allocation.
#pragma once
#include <cstdint>

namespace cd::media {

inline std::int64_t tickSample(std::int64_t n, std::uint32_t rate, int fps) noexcept {
    return (n * std::int64_t(rate) + fps / 2) / fps;
}

class AvSyncModel {
public:
    explicit AvSyncModel(std::uint32_t sampleRate = 48000) noexcept : rate_(sampleRate ? sampleRate : 48000) {}
    void record(std::int64_t tickSampleIdx, std::int64_t contentSampleIdx) noexcept;
    std::int64_t count() const noexcept { return n_; }
    double maxAbsSkewMs() const noexcept { return maxAbs_; }
    double meanSkewMs() const noexcept { return n_ ? sy_ / double(n_) : 0.0; }
    // Skew change over the observed span (ms): slope(ms/s) * span(s).
    double driftMs() const noexcept;
private:
    std::uint32_t rate_;
    std::int64_t n_ = 0;
    double sx_ = 0, sy_ = 0, sxy_ = 0, sxx_ = 0, maxAbs_ = 0, minX_ = 0, maxX_ = 0;
};

class ClockMapper {
public:
    explicit ClockMapper(std::uint32_t nominalRate) noexcept : nominal_(double(nominalRate)) {}
    void observe(double wallSeconds, std::int64_t sampleIndex) noexcept;
    // Estimated sample index at the given wall time (nominal-rate extrapolation until 2 points).
    std::int64_t sampleAt(double wallSeconds) const noexcept;
private:
    static constexpr int kN = 64;
    double wall_[kN] = {};
    double smp_[kN] = {};
    int count_ = 0, head_ = 0;
    double nominal_;
};

}  // namespace cd::media
