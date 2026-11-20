// CircuitDrift media: PNG screenshot encoder (CD-F03). RGBA8, filter 0, zlib stream made of
// stored (uncompressed) deflate blocks: always valid, deterministic, no dependency. CRC-32 per
// chunk and Adler-32 over the raw scanlines. Worker-thread only (allocates).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cd::media {

struct PngLimits {
    std::uint32_t maxDimension = 4096;            // per axis
    std::size_t maxRawBytes = 64u * 1024u * 1024u;  // h * (1 + 4w); also bounds the output
};

std::uint32_t crc32(const std::uint8_t* data, std::size_t n, std::uint32_t seed = 0) noexcept;
std::uint32_t adler32(const std::uint8_t* data, std::size_t n, std::uint32_t seed = 1) noexcept;

// `rgba` holds exactly width*height*4 bytes (row-major, tightly packed, non-premultiplied).
// Output dimensions are exactly width x height; zero or over-limit sizes are rejected with
// a message. `out` is replaced only on success.
bool encodePng(const std::uint8_t* rgba, std::size_t rgbaBytes, std::uint32_t width, std::uint32_t height,
               std::vector<std::uint8_t>& out, std::string& error, const PngLimits& lim = {});

}  // namespace cd::media
