// CircuitDrift media: silent animated GIF89a encoder (CD-F03). Worker-thread only (allocates).
// - Palette: exact when a frame has <= 256 distinct RGB colours (then decoding is pixel exact),
//   otherwise deterministic median cut (ties broken by packed colour value, no dithering).
//   Alpha is ignored (frames are opaque captures).
// - Global colour table = first frame's palette; later frames reuse it when every colour is in it,
//   otherwise they carry a local colour table.
// - LZW with clear code at table-full (code size reset), NETSCAPE2.0 loop extension (optional),
//   fixed per-frame delay in centiseconds (default 5 = 50 ms), disposal "do not dispose".
// - Exact selected dimensions; caps on dimension, frame count and output bytes.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cd::media {

struct GifLimits {
    std::uint32_t maxDimension = 1920;
    std::uint32_t maxFrames = 300;                      // 15 s at 20 FPS
    std::size_t maxBytes = 64u * 1024u * 1024u;
};
struct GifOptions {
    std::uint16_t delayCentiseconds = 5;                // 50 ms
    bool loop = true;                                   // false: no loop extension (play once)
};

// Deterministic quantiser, exposed for tests. palette is RGB triplets (size = 3*count, count<=256).
struct QuantizedFrame {
    std::vector<std::uint8_t> palette;
    std::vector<std::uint8_t> indices;  // one per pixel
    bool exact = false;                 // true when no colour was merged
};
QuantizedFrame quantizeRgba(const std::uint8_t* rgba, std::size_t pixelCount, unsigned maxColors = 256);

class GifWriter {
public:
    bool begin(std::uint32_t width, std::uint32_t height, const GifOptions& opt, const GifLimits& lim, std::string& error);
    bool addFrame(const std::uint8_t* rgba, std::size_t rgbaBytes, std::string& error);
    // Appends the trailer; the finished file is moved to `out`. Requires >= 1 frame.
    bool finish(std::vector<std::uint8_t>& out, std::string& error);
    std::uint32_t frameCount() const noexcept { return frames_; }
    std::size_t bytesSoFar() const noexcept { return buf_.size(); }

private:
    GifOptions opt_;
    GifLimits lim_;
    std::uint32_t w_ = 0, h_ = 0, frames_ = 0;
    bool begun_ = false, finished_ = false, failed_ = false;
    std::vector<std::uint8_t> buf_;
    std::vector<std::uint8_t> global_;  // padded global table (RGB)
    unsigned globalBits_ = 1;
};

}  // namespace cd::media
