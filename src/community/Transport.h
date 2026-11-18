// CircuitDrift community: injected transport, typed results, retry/backoff policy (CD-F04, gate G06).
// No socket/TLS code lives here. The plugin layer supplies a Transport (system HTTPS stack) and a
// TokenProvider (OAuth/PKCE + OS credential store, G06). This library never stores, logs or
// echoes credentials: tokens exist only in a request header at the moment of sending and are
// scrubbed from every message that reaches a Result.
// Threading: worker threads only; never audio.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cd::community {

enum class HttpMethod : unsigned char { Get, Post, Patch, Delete };
using Headers = std::vector<std::pair<std::string, std::string>>;

struct HttpRequest {
    HttpMethod method = HttpMethod::Get;
    std::string target;                         // "/v1/presets/..." (API) or an absolute https:// blob URL
    Headers query;                              // the Transport must percent-encode names/values
    Headers headers;
    std::string body;
    int connectTimeoutMs = 10000;               // spec: 10 s connect / 30 s total
    int totalTimeoutMs = 30000;
    std::size_t maxResponseBytes = 1u << 20;    // the Transport should stop reading beyond this
    const std::atomic<bool>* cancel = nullptr;  // panel close; Transport should poll
};
struct HttpResponse {
    int status = 0;
    Headers headers;
    std::string body;
    bool timedOut = false, cancelled = false, transportFailed = false;
};
class Transport {
public:
    virtual ~Transport() = default;
    virtual HttpResponse send(const HttpRequest& request) = 0;  // blocking; worker thread
};
class TokenProvider {
public:
    virtual ~TokenProvider() = default;
    // Returns false when no signed-in account/token is available. forceRefresh: the previous token was rejected (401).
    virtual bool accessToken(std::string& out, bool forceRefresh) = 0;
};

enum class Status : unsigned char {
    Ok, Created, NoContent, NotModified,
    BadRequest, Unauthorized, Forbidden, NotFound, Conflict, PreconditionFailed, PayloadTooLarge, Unprocessable, RateLimited,
    ServerError, BadGateway, ServiceUnavailable, GatewayTimeout,
    Timeout, TransportError, Cancelled, ResponseTooLarge, MalformedResponse, UnexpectedStatus,
    DigestMismatch, ValidationFailed, Incompatible, OfflineMiss, CacheCorrupt,
    AuthRequired, NotOwner, InvalidArgument, PublishNotConfirmed, Busy
};
const char* statusName(Status s) noexcept;

struct Result {
    Status status = Status::Ok;
    int httpStatus = 0;
    int retryAfterSeconds = 0;     // from Retry-After / error.retryAfter, 0 if absent
    std::string code, message, requestId;  // server-provided text, sanitised and credential-scrubbed
    bool ok() const noexcept { return status == Status::Ok || status == Status::Created || status == Status::NoContent || status == Status::NotModified; }
};
bool isRetryableStatus(Status s) noexcept;  // Timeout, TransportError, RateLimited, 500, 502, 503, 504

// Removes control characters, truncates to maxLen, and replaces any occurrence of `secret`
// (and "Bearer <token>" shapes) with "[redacted]".
std::string sanitizeText(const std::string& s, const std::string& secret, std::size_t maxLen = 256);
// Case-insensitive header lookup; returns nullptr if absent.
const std::string* findHeader(const Headers& h, const std::string& name) noexcept;
// Maps an HTTP outcome to a typed result (200/201/204/304/400/401/403/404/409/412/413/422/429/500/502/503/504,
// timeouts, transport failure, cancellation). Parses the error body {requestId, schemaVersion, error:{code,message,retryAfter}}
// defensively: malformed error bodies never change the status mapping.
Result mapHttpResponse(const HttpResponse& r, const std::string& secretToScrub = {});

struct RetryPolicy {
    int maxRetries = 3;                          // reads: 1/2/4 s + bounded jitter
    int baseDelayMs[3] = {1000, 2000, 4000};
    double jitterFraction = 0.25;                // up to +25 %
    int maxRetryAfterSeconds = 60;               // longer server requests are not waited for
    bool shouldRetry(Status s, int attempt, int retryAfterSeconds) const noexcept;
    // unit01: caller-supplied random in [0,1] (injected for determinism). Retry-After wins when larger.
    int delayMs(int attempt, int retryAfterSeconds, double unit01) const noexcept;
};

// Remembers a server-requested pause (429/503 Retry-After) so no request is sent before it ends.
class RateLimitGate {
public:
    void note(std::int64_t nowMs, int retryAfterSeconds, int maxSeconds = 60) noexcept;
    bool blocked(std::int64_t nowMs, std::int64_t& remainingMs) const noexcept;
private:
    std::int64_t until_ = 0;
};

}  // namespace cd::community
