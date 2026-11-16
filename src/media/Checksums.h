// CircuitDrift media: CRC-32 (PNG/ISO 3309, poly 0xEDB88320) and Adler-32 (RFC 1950),
// implemented from the specifications. Owner: cd_media. Worker-thread use; pure functions.
#pragma once
#include <cstddef>
#include <cstdint>

namespace cd::media {

class Crc32 {
public:
    void update(const void* data, std::size_t n) noexcept;
    std::uint32_t value() const noexcept { return ~state_; }
private:
    std::uint32_t state_ = 0xFFFFFFFFu;
};

class Adler32 {
public:
    void update(const void* data, std::size_t n) noexcept;
    std::uint32_t value() const noexcept { return (b_ << 16) | a_; }
private:
    std::uint32_t a_ = 1, b_ = 0;
};

}  // namespace cd::media
