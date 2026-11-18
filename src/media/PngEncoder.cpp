#include "media/PngEncoder.h"
#include <algorithm>
#include <array>
#include <limits>

namespace cd::media {
namespace {
const std::array<std::uint32_t, 256>& crcTable() {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[n] = c;
        }
        return t;
    }();
    return table;
}
void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back(std::uint8_t(x >> 24)); v.push_back(std::uint8_t(x >> 16));
    v.push_back(std::uint8_t(x >> 8)); v.push_back(std::uint8_t(x));
}
void chunk(std::vector<std::uint8_t>& out, const char type[5], const std::vector<std::uint8_t>& data) {
    put32(out, std::uint32_t(data.size()));
    const std::size_t typeAt = out.size();
    for (int i = 0; i < 4; ++i) out.push_back(std::uint8_t(type[i]));
    out.insert(out.end(), data.begin(), data.end());
    put32(out, crc32(out.data() + typeAt, 4 + data.size()));
}
}  // namespace

std::uint32_t crc32(const std::uint8_t* data, std::size_t n, std::uint32_t seed) noexcept {
    const auto& t = crcTable();
    std::uint32_t c = ~seed;
    for (std::size_t i = 0; i < n; ++i) c = t[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    return ~c;
}

std::uint32_t adler32(const std::uint8_t* data, std::size_t n, std::uint32_t seed) noexcept {
    std::uint32_t a = seed & 0xFFFFu, b = seed >> 16;
    while (n) {
        const std::size_t k = std::min<std::size_t>(n, 5552);  // largest run without 32-bit overflow
        for (std::size_t i = 0; i < k; ++i) { a += data[i]; b += a; }
        a %= 65521u; b %= 65521u;
        data += k; n -= k;
    }
    return (b << 16) | a;
}

bool encodePng(const std::uint8_t* rgba, std::size_t rgbaBytes, std::uint32_t width, std::uint32_t height,
               std::vector<std::uint8_t>& out, std::string& error, const PngLimits& lim) {
    if (width == 0 || height == 0) { error = "PNG dimensions must be non-zero"; return false; }
    if (width > lim.maxDimension || height > lim.maxDimension) { error = "PNG dimensions exceed limit"; return false; }
    const std::uint64_t rowBytes = 1ull + 4ull * width;
    const std::uint64_t rawBytes = rowBytes * height;  // <= (4096*4+1)*4096, no overflow with uint64
    if (rawBytes > lim.maxRawBytes) { error = "PNG raw size exceeds limit"; return false; }
    if (!rgba || rgbaBytes != std::uint64_t(width) * height * 4ull) { error = "pixel buffer size does not match dimensions"; return false; }

    std::vector<std::uint8_t> z;
    z.reserve(std::size_t(rawBytes) + std::size_t(rawBytes / 65535 + 1) * 5 + 6);
    z.push_back(0x78); z.push_back(0x01);  // CMF/FLG: deflate, 32K window, no preset dict, FCHECK ok (0x7801 % 31 == 0)
    // Stream the scanlines through stored blocks of <= 65535 bytes.
    std::vector<std::uint8_t> block;
    block.reserve(65535);
    std::uint32_t adler = 1;
    auto flush = [&](bool last) {
        const std::uint16_t len = std::uint16_t(block.size());
        z.push_back(last ? 1 : 0);
        z.push_back(std::uint8_t(len)); z.push_back(std::uint8_t(len >> 8));
        z.push_back(std::uint8_t(~len)); z.push_back(std::uint8_t((~len) >> 8));
        z.insert(z.end(), block.begin(), block.end());
        block.clear();
    };
    std::uint64_t emitted = 0;
    auto push = [&](const std::uint8_t* p, std::size_t n) {
        adler = adler32(p, n, adler);
        while (n) {
            const std::size_t room = 65535 - block.size();
            const std::size_t k = std::min(room, n);
            block.insert(block.end(), p, p + k);
            p += k; n -= k; emitted += k;
            if (block.size() == 65535 && emitted < rawBytes) flush(false);
        }
    };
    const std::uint8_t filter = 0;
    for (std::uint32_t y = 0; y < height; ++y) {
        push(&filter, 1);
        push(rgba + std::size_t(y) * width * 4, std::size_t(width) * 4);
    }
    flush(true);
    put32(z, adler);

    std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<std::uint8_t> ihdr;
    put32(ihdr, width); put32(ihdr, height);
    ihdr.push_back(8); ihdr.push_back(6); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);  // 8-bit RGBA, no interlace
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    out = std::move(png);
    error.clear();
    return true;
}

}  // namespace cd::media
