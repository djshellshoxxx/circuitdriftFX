#include "media/AvSync.h"
#include <algorithm>
#include <cmath>

namespace cd::media {

std::int64_t videoFramePtsSamples(std::uint64_t frameIndex, double sampleRate, unsigned fps) noexcept {
    if (fps == 0 || !(sampleRate > 0)) return 0;
    return std::int64_t(std::floor(double(frameIndex) * sampleRate / double(fps) + 0.5));
}
double samplesToMs(std::int64_t samples, double sampleRate) noexcept {
    return sampleRate > 0 ? double(samples) * 1000.0 / sampleRate : 0.0;
}
std::int64_t samplesToMicroseconds(std::int64_t samples, double sampleRate) noexcept {
    return sampleRate > 0 ? std::int64_t(std::llround(double(samples) * 1.0e6 / sampleRate)) : 0;
}

AvReport analyzeAvTimestamps(const std::vector<std::int64_t>& pts, double fs, unsigned fps, std::int64_t latency) {
    AvReport r;
    r.frames = pts.size();
    if (pts.empty() || fps == 0 || !(fs > 0)) return r;
    double first = 0, last = 0;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        if (i > 0 && pts[i] <= pts[i - 1]) r.monotonic = false;
        const double actualMs = samplesToMs(pts[i] - latency, fs);
        const double nominalMs = double(i) * 1000.0 / double(fps);
        const double skew = actualMs - nominalMs;
        if (i == 0) first = skew;
        last = skew;
        r.maxAbsSkewMs = std::max(r.maxAbsSkewMs, std::abs(skew));
    }
    r.driftMs = last - first;
    return r;
}

std::vector<PacedFrame> paceFrames(const std::vector<std::int64_t>& captured, double fs, std::uint64_t outputFrames, unsigned fps) {
    std::vector<PacedFrame> out;
    constexpr std::uint64_t kCap = 1u << 20;
    out.reserve(std::size_t(std::min(outputFrames, kCap)));
    std::size_t newest = 0;
    bool have = false, haveLast = false;
    std::size_t last = 0;
    for (std::uint64_t i = 0; i < outputFrames && i < kCap; ++i) {
        const std::int64_t due = videoFramePtsSamples(i, fs, fps);
        while (newest < captured.size() && captured[newest] <= due) { have = true; ++newest; }
        if (!have) { out.push_back({i, 0, due, true}); continue; }  // nothing captured yet: hold first frame
        const std::size_t k = newest - 1;
        const bool repeat = haveLast && k == last;
        out.push_back({i, k, due, repeat});
        last = k; haveLast = true;
    }
    return out;
}

bool exportTransitionAllowed(ExportState from, ExportState to) noexcept {
    using S = ExportState;
    switch (from) {
        case S::Idle: return to == S::Armed;
        case S::Armed: return to == S::Recording || to == S::Cancelled || to == S::Failed;
        case S::Recording: return to == S::Finalizing || to == S::Cancelled || to == S::Failed;
        case S::Finalizing: return to == S::Completed || to == S::Cancelled || to == S::Failed;
        case S::Completed: case S::Cancelled: case S::Failed: return to == S::Idle;
    }
    return false;
}
const char* exportStateName(ExportState s) noexcept {
    switch (s) {
        case ExportState::Idle: return "Idle"; case ExportState::Armed: return "Armed";
        case ExportState::Recording: return "Recording"; case ExportState::Finalizing: return "Finalizing";
        case ExportState::Completed: return "Completed"; case ExportState::Cancelled: return "Cancelled";
        case ExportState::Failed: return "Failed";
    }
    return "?";
}

ExportSize exportSize(ExportFormat f, Framing fr) noexcept {
    const bool gif = f == ExportFormat::Gif;
    switch (fr) {
        case Framing::Portrait9x16: return gif ? ExportSize{540, 960} : ExportSize{1080, 1920};
        case Framing::Square1x1: return gif ? ExportSize{540, 540} : ExportSize{1080, 1080};
        case Framing::Landscape16x9: return gif ? ExportSize{960, 540} : ExportSize{1920, 1080};
    }
    return {1080, 1920};
}
double maxDurationSeconds(ExportFormat f) noexcept { return f == ExportFormat::Mp4 ? 60.0 : f == ExportFormat::Gif ? 15.0 : 0.0; }
unsigned exportFps(ExportFormat f) noexcept { return f == ExportFormat::Mp4 ? 30u : f == ExportFormat::Gif ? 20u : 0u; }

}  // namespace cd::media
