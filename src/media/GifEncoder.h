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

// Conservative encoder working-storage admission bound: one reserved output buffer,
// peak quantizer scratch and 128 KiB for fixed tables/bookkeeping. Caller-owned RGBA,
// queue/render buffers, allocator overhead and platform/thread storage are excluded.
// Returns SIZE_MAX for unsupported dimensions, byte capacity or arithmetic overflow.
// The bound applies to one active writer; finish transfers, rather than copies, output.
std::size_t gifEncodingMemoryBound(std::uint32_t width, std::uint32_t height, std::size_t outputBytes) noexcept;

class GifWriter {
public:
    GifWriter() = default;
    // One owner preserves the pre-reserved capacity and encoded-frame offsets.
    GifWriter(const GifWriter&) = delete;
    GifWriter& operator=(const GifWriter&) = delete;
    // Reserves the complete maxBytes output budget once. Subsequent writes cannot
    // geometrically grow output storage; every emitted byte is admitted first.
    bool begin(std::uint32_t width, std::uint32_t height, const GifOptions& opt, const GifLimits& lim, std::string& error);
    bool addFrame(const std::uint8_t* rgba, std::size_t rgbaBytes, std::string& error);
    // Reuses the last encoded image block, including its palette and selected delay,
    // without retaining RGBA or quantizing again. Counts toward both output/frame caps.
    bool repeatLastFrame(std::string& error);
    // Appends the trailer within maxBytes; moves the file to `out` only on success.
    // Requires >= 1 frame. Failure leaves caller-owned `out` unchanged.
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
    std::size_t lastFrameOffset_ = 0, lastFrameBytes_ = 0;
};

}  // namespace cd::media
