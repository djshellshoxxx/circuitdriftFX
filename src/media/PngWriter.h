// CircuitDrift media: PNG writer (PNG spec / RFC 2083). Owner: cd_media. Worker thread only.
// 8-bit RGBA (color type 6), filter 0, no interlace. Pixel data is a zlib stream made of STORED
// (uncompressed) deflate blocks with Adler-32, so no third-party compressor is needed; files are
// larger than compressed PNGs by design (a compressor can replace the block emitter later).
// Streams to the sink with bounded scratch memory (~64 KiB). Does not commit the sink.
#pragma once
#include "media/ByteSink.h"
#include "media/MediaTypes.h"
#include <cstdint>

namespace cd::media {

inline constexpr int kMaxImageDim = 16384;

// rgba: width*height*4 bytes, row-major, straight alpha. Exact dimensions are written to IHDR.
MediaError writePng(ByteSink& sink, const std::uint8_t* rgba, int width, int height);

}  // namespace cd::media
