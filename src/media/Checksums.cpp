#include "media/Checksums.h"
#include <array>

namespace cd::media {
namespace {
constexpr std::array<std::uint32_t, 256> makeTable() {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t n = 0; n < 256; ++n) {
        std::uint32_t c = n;
        for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        t[n] = c;
    }
    return t;
}
constexpr auto kTable = makeTable();
}  // namespace

void Crc32::update(const void* data, std::size_t n) noexcept {
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::uint32_t c = state_;
    for (std::size_t i = 0; i < n; ++i) c = kTable[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    state_ = c;
}

void Adler32::update(const void* data, std::size_t n) noexcept {
    const auto* p = static_cast<const std::uint8_t*>(data);
    constexpr std::uint32_t kMod = 65521;
    while (n > 0) {
        const std::size_t chunk = n < 5552 ? n : 5552;  // largest n with no 32-bit overflow
        for (std::size_t i = 0; i < chunk; ++i) { a_ += p[i]; b_ += a_; }
        a_ %= kMod; b_ %= kMod;
        p += chunk; n -= chunk;
    }
}
}  // namespace cd::media
