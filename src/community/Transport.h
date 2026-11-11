// CircuitDrift community: abstract transport, clock, RNG and auth-token types used by CatalogClient.
// Gate G06 (backend/provider/OAuth) is OPEN: this module contains NO network code, NO sockets and NO
// credentials. A real HTTPS transport is a future adapter implementing Transport outside this module.
// Owner module: cd_community. Threading: worker threads only, never the audio thread.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cd::community {

// Bearer token. Deliberately NOT serializable: no operator<<, redacted toString(), no accessor other
// than headerValue(), which CatalogClient uses solely to fill the Authorization transport header.
class AuthToken {
public:
    AuthToken() = default;
    explicit AuthToken(std::string secret) : secret_(std::move(secret)) {}
    bool empty() const noexcept { return secret_.empty(); }
    std::string toString() const { return empty() ? "AuthToken(empty)" : "AuthToken([REDACTED])"; }
    std::string headerValue() const { return "Bearer " + secret_; }

private:
    std::string secret_;
};
std::ostream& operator<<(std::ostream&, const AuthToken&) = delete;

// Cooperative cancellation (e.g. panel close). Safe to set from any thread.
class CancelToken {
public:
    void cancel() noexcept { flag_.store(true, std::memory_order_release); }
    bool cancelled() const noexcept { return flag_.load(std::memory_order_acquire); }

private:
    std::atomic<bool> flag_{false};
};

enum class HttpMethod : unsigned char { Get, Post, Patch, Delete };
using Headers = std::vector<std::pair<std::string, std::string>>;

struct TransportRequest {
    HttpMethod method = HttpMethod::Get;
    std::string target;   // "/v1/..." path, or an absolute https:// blob URL for downloads
    Headers query;        // decoded key/value pairs; the transport encodes them
    Headers headers;      // may include Authorization (the only place a token appears)
    std::string body;     // JSON; never contains credentials
    int connectTimeoutMs = 10000;
    int totalTimeoutMs = 30000;
    std::size_t maxResponseBytes = 0;  // transport must abort and report TooLarge beyond this
};

enum class TransportOutcome : unsigned char { Completed, Timeout, Cancelled, NetworkError, TooLarge };

struct TransportResponse {
    TransportOutcome outcome = TransportOutcome::Completed;
    int status = 0;
    Headers headers;
    std::string body;
};

class Transport {
public:
    virtual ~Transport() = default;
    // Blocking; must honour the timeouts and poll `cancel`. May be called from two threads at once.
    virtual TransportResponse send(const TransportRequest& request, const CancelToken& cancel) = 0;
};

class Clock {
public:
    virtual ~Clock() = default;
    virtual std::int64_t nowMs() = 0;
    // Returns false when cancelled before the delay elapsed.
    virtual bool sleepMs(std::int64_t ms, const CancelToken& cancel) = 0;
};

// Real clock: steady time, sleeps in 10 ms slices so cancellation is prompt. No network.
class SystemClock final : public Clock {
public:
    std::int64_t nowMs() override;
    bool sleepMs(std::int64_t ms, const CancelToken& cancel) override;
};

class Rng {
public:
    virtual ~Rng() = default;
    virtual std::uint32_t next() = 0;
};

// Deterministic xorshift RNG for jitter (tests) and idempotency-key material in tests.
class SeededRng final : public Rng {
public:
    explicit SeededRng(std::uint64_t seed) : s_(seed ? seed : 0x9e3779b97f4a7c15ull) {}
    std::uint32_t next() override {
        s_ ^= s_ << 13; s_ ^= s_ >> 7; s_ ^= s_ << 17;
        return static_cast<std::uint32_t>(s_ >> 16);
    }

private:
    std::uint64_t s_;
};

}  // namespace cd::community
