// CircuitDrift community: SHA-256 (FIPS 180-4), original implementation written for this project
// from the standard (no third-party code). Used for immutable blob digests over exact bytes.
// Owner module: cd_community. Threading: pure, reentrant; worker/UI threads only by policy.
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
    void update(const void* data, std::size_t len) noexcept;
    void update(std::string_view s) noexcept { update(s.data(), s.size()); }
    std::array<std::uint8_t, 32> finish() noexcept;  // reset() before reuse

private:
    void block(const std::uint8_t* p) noexcept;
    std::uint32_t h_[8];
    std::uint8_t buf_[64];
    std::size_t bufLen_ = 0;
    std::uint64_t totalBytes_ = 0;
};

std::array<std::uint8_t, 32> sha256(std::string_view data) noexcept;
std::string toHex(const std::array<std::uint8_t, 32>& d);
std::string sha256Hex(std::string_view data);   // lowercase, 64 chars
bool isSha256Hex(std::string_view s) noexcept;  // exactly 64 lowercase hex chars

}  // namespace cd::community
