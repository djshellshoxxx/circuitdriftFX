// CircuitDrift media: A/V timestamp bookkeeping and exporter state/presets (CD-F03, T03).
// Video PTS are audio sample indices (spec). Skew and drift are computed against the nominal
// 30 FPS video clock derived from the audio clock; budgets: skew <= 33.4 ms, 60 s drift <= 20 ms.
// Pure functions/classes, worker or test use.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cd::media {

constexpr double kMaxSkewMs = 33.4;
constexpr double kMaxDriftMs = 20.0;

// Nominal sample index of video frame `i` at `fps` for sample rate `fs` (round half up).
std::int64_t videoFramePtsSamples(std::uint64_t frameIndex, double sampleRate, unsigned fps = 30) noexcept;
double samplesToMs(std::int64_t samples, double sampleRate) noexcept;
std::int64_t samplesToMicroseconds(std::int64_t samples, double sampleRate) noexcept;

struct AvReport {
    bool monotonic = true;       // strictly increasing video PTS
    std::uint64_t frames = 0;
    double maxAbsSkewMs = 0;     // max |frame time derived from PTS - nominal frame time|
    double driftMs = 0;          // (skew at last frame) - (skew at first frame)
    bool withinBudget() const noexcept { return monotonic && maxAbsSkewMs <= kMaxSkewMs && std::abs(driftMs) <= kMaxDriftMs; }
};
// videoPts: sample-index PTS of each video frame (frame 0 expected near sample 0).
// audioLatencySamples: output-tap latency subtracted from nominal (already-declared latency compensation).
AvReport analyzeAvTimestamps(const std::vector<std::int64_t>& videoPts, double sampleRate, unsigned fps = 30,
                             std::int64_t audioLatencySamples = 0);

// Encoder-behind bookkeeping: counts repeated frames; warn above 2% (spec).
class RepeatStats {
public:
    void frame(bool repeated) noexcept { ++total_; if (repeated) ++repeated_; }
    std::uint64_t total() const noexcept { return total_; }
    std::uint64_t repeated() const noexcept { return repeated_; }
    double fraction() const noexcept { return total_ ? double(repeated_) / double(total_) : 0.0; }
    bool warn() const noexcept { return fraction() > 0.02; }
private:
    std::uint64_t total_ = 0, repeated_ = 0;
};

// Maps captured frames onto the 30 FPS grid: for output frame i it picks the newest captured
// frame with pts <= nominal(i); if that frame was already used, the output is a repeat.
struct PacedFrame { std::uint64_t outputIndex; std::size_t sourceIndex; std::int64_t ptsSamples; bool repeated; };
std::vector<PacedFrame> paceFrames(const std::vector<std::int64_t>& capturedPts, double sampleRate,
                                   std::uint64_t outputFrames, unsigned fps = 30);

// ---- Exporter state machine and presets ----
enum class ExportState : unsigned char { Idle, Armed, Recording, Finalizing, Completed, Cancelled, Failed };
bool exportTransitionAllowed(ExportState from, ExportState to) noexcept;
const char* exportStateName(ExportState s) noexcept;

enum class ExportFormat : unsigned char { Png, Gif, Mp4 };
enum class Framing : unsigned char { Portrait9x16, Square1x1, Landscape16x9 };
struct ExportSize { std::uint32_t width, height; };
// Spec sizes: MP4 1080x1920 / 1080x1080 / 1920x1080; GIF 540x960 / 540x540 / 960x540; PNG uses the MP4 sizes.
ExportSize exportSize(ExportFormat f, Framing framing) noexcept;
double maxDurationSeconds(ExportFormat f) noexcept;  // MP4 60 s, GIF 15 s, PNG 0
unsigned exportFps(ExportFormat f) noexcept;         // MP4 30, GIF 20, PNG 0

}  // namespace cd::media
