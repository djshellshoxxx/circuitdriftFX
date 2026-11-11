#include "media/PngWriter.h"
#include "media/Checksums.h"
#include <algorithm>
#include <cstring>
#include <vector>

namespace cd::media {
namespace {

void be32(std::uint8_t* p, std::uint32_t v) {
    p[0] = std::uint8_t(v >> 24); p[1] = std::uint8_t(v >> 16); p[2] = std::uint8_t(v >> 8); p[3] = std::uint8_t(v);
}

// Writes data bytes of one chunk while accumulating the CRC (type + data).
class ChunkOut {
public:
    ChunkOut(ByteSink& s, const char type[4], std::uint32_t length) : sink_(s) {
        std::uint8_t hdr[8];
        be32(hdr, length);
        std::memcpy(hdr + 4, type, 4);
        ok_ = sink_.write(hdr, 8);
        crc_.update(type, 4);
    }
    void data(const void* p, std::size_t n) {
        if (!ok_) return;
        ok_ = sink_.write(p, n);
        crc_.update(p, n);
    }
    bool finish() {
        if (!ok_) return false;
        std::uint8_t c[4];
        be32(c, crc_.value());
        return sink_.write(c, 4);
    }
private:
    ByteSink& sink_;
    Crc32 crc_;
    bool ok_ = true;
};

bool smallChunk(ByteSink& s, const char type[4], const std::uint8_t* d, std::uint32_t n) {
    ChunkOut c(s, type, n);
    if (n) c.data(d, n);
    return c.finish();
}

}  // namespace

MediaError writePng(ByteSink& sink, const std::uint8_t* rgba, int width, int height) {
    if (!rgba || width < 1 || height < 1 || width > kMaxImageDim || height > kMaxImageDim)
        return MediaError::InvalidArgument;
    const std::uint64_t stride = 1 + std::uint64_t(width) * 4;
    const std::uint64_t raw = stride * std::uint64_t(height);
    const std::uint64_t blocks = (raw + 65534) / 65535;
    const std::uint64_t idatLen = 2 + raw + 5 * blocks + 4;
    if (idatLen > 0x7FFFFFFFull) return MediaError::InvalidArgument;  // PNG chunk length limit

    static const std::uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (!sink.write(sig, 8)) return MediaError::SinkWriteFailed;
    std::uint8_t ihdr[13];
    be32(ihdr, std::uint32_t(width)); be32(ihdr + 4, std::uint32_t(height));
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    if (!smallChunk(sink, "IHDR", ihdr, 13)) return MediaError::SinkWriteFailed;

    ChunkOut idat(sink, "IDAT", std::uint32_t(idatLen));
    const std::uint8_t zhdr[2] = {0x78, 0x01};  // deflate, 32K window, no preset dict
    idat.data(zhdr, 2);
    Adler32 adler;
    std::vector<std::uint8_t> block(5 + 65535);
    std::uint64_t pos = 0;
    while (pos < raw) {
        const std::uint32_t len = std::uint32_t(std::min<std::uint64_t>(65535, raw - pos));
        std::uint8_t* out = block.data() + 5;
        std::uint32_t filled = 0;
        while (filled < len) {  // assemble [filter byte | row pixels] stream
            const std::uint64_t p = pos + filled;
            const std::uint64_t row = p / stride, off = p % stride;
            if (off == 0) { out[filled++] = 0; continue; }
            const std::uint32_t n = std::uint32_t(std::min<std::uint64_t>(len - filled, stride - off));
            std::memcpy(out + filled, rgba + row * std::uint64_t(width) * 4 + (off - 1), n);
            filled += n;
        }
        adler.update(out, len);
        block[0] = (pos + len == raw) ? 1 : 0;  // BFINAL, BTYPE=00 (stored)
        block[1] = std::uint8_t(len); block[2] = std::uint8_t(len >> 8);
        block[3] = std::uint8_t(~len); block[4] = std::uint8_t((~len) >> 8);
        idat.data(block.data(), 5 + len);
        pos += len;
    }
    std::uint8_t ad[4];
    be32(ad, adler.value());
    idat.data(ad, 4);
    if (!idat.finish()) return MediaError::SinkWriteFailed;
    if (!smallChunk(sink, "IEND", nullptr, 0)) return MediaError::SinkWriteFailed;
    return MediaError::Ok;
}

}  // namespace cd::media
