// CircuitDrift media: GIF89a writer with LZW (GIF89a spec, 1990). Owner: cd_media. Worker only.
// Quantization: FIXED 6x7x6 colour cube (252 entries: 6 red x 7 green x 6 blue levels, nearest
// level, no dithering); palette slots 252..255 are black. Alpha is ignored (treated opaque).
// Global colour table, min LZW code size 8, deferred clear at 4096 codes, one full-size frame
// per image (disposal 1), per-frame delay in centiseconds (5 cs = 50 ms), optional NETSCAPE2.0
// infinite-loop extension. Streams to the sink; scratch memory is O(1) plus one quantized frame.
// Does not commit the sink.
#pragma once
#include "media/ByteSink.h"
#include "media/MediaTypes.h"
#include <cstdint>
#include <vector>

namespace cd::media {

class GifWriter {
public:
    GifWriter(ByteSink& sink, int width, int height, int delayCentiseconds, bool loop);
    MediaError begin();                                  // header, logical screen, palette, loop ext
    MediaError addFrameRgba(const std::uint8_t* rgba);   // quantizes then encodes
    MediaError addFrameIndexed(const std::uint8_t* idx); // width*height palette indices
    MediaError finish();                                 // trailer
    int framesWritten() const noexcept { return frames_; }

    static std::uint8_t quantize(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept;
    static void paletteColor(int index, std::uint8_t rgb[3]) noexcept;

private:
    struct Slot { std::int32_t key; std::int32_t code; };
    ByteSink& sink_;
    int w_, h_, delay_;
    bool loop_;
    int frames_ = 0;
    bool began_ = false, failed_ = false;
    std::vector<std::uint8_t> idx_;
    std::vector<Slot> table_;
    std::uint8_t sub_[256];  // [len][data...] sub-block under construction
    int subLen_ = 0;
    std::uint32_t bitBuf_ = 0;
    int bitCnt_ = 0;

    void put(const void* p, std::size_t n);
    void emitCode(int code, int size);
    void flushSub();
    void resetTable();
    void encodeLzw(const std::uint8_t* idx);
};

}  // namespace cd::media
