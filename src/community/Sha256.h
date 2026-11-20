// CircuitDrift community: SHA-256 (FIPS 180-4). Used for immutable revision digests over the exact
// downloaded bytes and for cache integrity. Self-contained, no OS crypto dependency.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace cd::community {

class Sha256 {
public:
    Sha256() noexcept { reset(); }
    void reset() noexcept;
    void update(const void* data, std::size_t n) noexcept;
    std::array<std::uint8_t, 32> finish() noexcept;  // finalises; call reset() to reuse
private:
    void block(const std::uint8_t* p) noexcept;
    std::uint32_t h_[8];
    std::uint8_t buf_[64];
    std::size_t bufLen_;
    std::uint64_t totalBytes_;
};

std::string toHex(const std::uint8_t* d, std::size_t n);
std::string sha256Hex(std::string_view data);
// 64 lowercase hex characters.
bool isSha256Hex(std::string_view s) noexcept;

}  // namespace cd::community
