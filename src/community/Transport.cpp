#include "community/Transport.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include "core/Json.h"

namespace cd::community {

const char* statusName(Status s) noexcept {
    switch (s) {
        case Status::Ok: return "ok"; case Status::Created: return "created"; case Status::NoContent: return "no-content";
        case Status::NotModified: return "not-modified"; case Status::BadRequest: return "bad-request";
        case Status::Unauthorized: return "unauthorized"; case Status::Forbidden: return "forbidden";
        case Status::NotFound: return "not-found"; case Status::Conflict: return "conflict";
        case Status::PreconditionFailed: return "precondition-failed"; case Status::PayloadTooLarge: return "payload-too-large";
        case Status::Unprocessable: return "unprocessable"; case Status::RateLimited: return "rate-limited";
        case Status::ServerError: return "server-error"; case Status::BadGateway: return "bad-gateway";
        case Status::ServiceUnavailable: return "service-unavailable"; case Status::GatewayTimeout: return "gateway-timeout";
        case Status::Timeout: return "timeout"; case Status::TransportError: return "transport-error";
        case Status::Cancelled: return "cancelled"; case Status::ResponseTooLarge: return "response-too-large";
        case Status::MalformedResponse: return "malformed-response"; case Status::UnexpectedStatus: return "unexpected-status";
        case Status::DigestMismatch: return "digest-mismatch"; case Status::ValidationFailed: return "validation-failed";
        case Status::Incompatible: return "incompatible"; case Status::OfflineMiss: return "offline-miss";
        case Status::CacheCorrupt: return "cache-corrupt"; case Status::AuthRequired: return "auth-required";
        case Status::NotOwner: return "not-owner"; case Status::InvalidArgument: return "invalid-argument";
        case Status::PublishNotConfirmed: return "publish-not-confirmed"; case Status::Busy: return "busy";
    }
    return "?";
}

bool isRetryableStatus(Status s) noexcept {
    return s == Status::Timeout || s == Status::TransportError || s == Status::RateLimited || s == Status::ServerError ||
           s == Status::BadGateway || s == Status::ServiceUnavailable || s == Status::GatewayTimeout;
}

namespace {
bool ieq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}
std::string replaceAll(std::string s, const std::string& what, const std::string& with) {
    if (what.empty()) return s;
    for (std::size_t at = 0; (at = s.find(what, at)) != std::string::npos; at += with.size()) s.replace(at, what.size(), with);
    return s;
}
bool parseSeconds(const std::string& v, int& out) {
    if (v.empty() || v.size() > 9) return false;
    long n = 0;
    for (char c : v) { if (c < '0' || c > '9') return false; n = n * 10 + (c - '0'); }
    out = int(n);
    return true;
}
}  // namespace

const std::string* findHeader(const Headers& h, const std::string& name) noexcept {
    for (const auto& kv : h) if (ieq(kv.first, name)) return &kv.second;
    return nullptr;
}

std::string sanitizeText(const std::string& in, const std::string& secret, std::size_t maxLen) {
    std::string s = replaceAll(in, secret.size() >= 4 ? secret : std::string(), "[redacted]");
    // "Bearer xyz" shapes: redact the credential following the word.
    for (std::size_t at = 0; at < s.size();) {
        std::string low = s.substr(at, 7);
        std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (low == "bearer ") {
            std::size_t e = at + 7;
            while (e < s.size() && !std::isspace(static_cast<unsigned char>(s[e]))) ++e;
            s.replace(at + 7, e - (at + 7), "[redacted]");
            at += 7 + 10;
        } else ++at;
    }
    std::string out;
    for (unsigned char c : s) {
        if (out.size() >= maxLen) break;
        out.push_back((c < 0x20 || c == 0x7F) ? ' ' : char(c));
    }
    return out;
}

Result mapHttpResponse(const HttpResponse& r, const std::string& secret) {
    Result res;
    res.httpStatus = r.status;
    if (r.cancelled) { res.status = Status::Cancelled; return res; }
    if (r.timedOut) { res.status = Status::Timeout; res.message = "request timed out"; return res; }
    if (r.transportFailed || r.status == 0) { res.status = Status::TransportError; res.message = "transport failure"; return res; }
    switch (r.status) {
        case 200: res.status = Status::Ok; break;
        case 201: res.status = Status::Created; break;
        case 204: res.status = Status::NoContent; break;
        case 304: res.status = Status::NotModified; break;
        case 400: res.status = Status::BadRequest; break;
        case 401: res.status = Status::Unauthorized; break;
        case 403: res.status = Status::Forbidden; break;
        case 404: res.status = Status::NotFound; break;
        case 409: res.status = Status::Conflict; break;
        case 412: res.status = Status::PreconditionFailed; break;
        case 413: res.status = Status::PayloadTooLarge; break;
        case 422: res.status = Status::Unprocessable; break;
        case 429: res.status = Status::RateLimited; break;
        case 500: res.status = Status::ServerError; break;
        case 502: res.status = Status::BadGateway; break;
        case 503: res.status = Status::ServiceUnavailable; break;
        case 504: res.status = Status::GatewayTimeout; break;
        default: res.status = r.status >= 500 && r.status < 600 ? Status::ServerError : Status::UnexpectedStatus; break;
    }
    if (const std::string* ra = findHeader(r.headers, "Retry-After")) { int s; if (parseSeconds(*ra, s)) res.retryAfterSeconds = s; }
    if (r.status >= 400 && !r.body.empty() && r.body.size() <= 65536) {
        core::JsonValue v; std::string err;
        core::JsonLimits lim; lim.maxBytes = 65536; lim.maxDepth = 8;
        if (core::parseJson(r.body, v, err, lim) && v.isObject()) {
            if (const auto* id = v.get("requestId"); id && id->isString()) res.requestId = sanitizeText(id->string, secret, 64);
            if (const auto* e = v.get("error"); e && e->isObject()) {
                if (const auto* c = e->get("code"); c && c->isString()) res.code = sanitizeText(c->string, secret, 64);
                if (const auto* m = e->get("message"); m && m->isString()) res.message = sanitizeText(m->string, secret, 256);
                if (const auto* ra = e->get("retryAfter"); ra && ra->isNumber() && ra->numberIsExactInteger && ra->number >= 0 && ra->number <= 86400 && res.retryAfterSeconds == 0)
                    res.retryAfterSeconds = int(ra->number);
            }
        }
    }
    return res;
}

bool RetryPolicy::shouldRetry(Status s, int attempt, int retryAfterSeconds) const noexcept {
    if (!isRetryableStatus(s) || attempt < 0 || attempt >= maxRetries) return false;
    if (retryAfterSeconds > maxRetryAfterSeconds) return false;  // server asked for a longer pause than we will wait
    return true;
}

int RetryPolicy::delayMs(int attempt, int retryAfterSeconds, double unit01) const noexcept {
    const int idx = std::clamp(attempt, 0, 2);
    const double u = std::isfinite(unit01) ? std::clamp(unit01, 0.0, 1.0) : 0.0;
    const double base = double(baseDelayMs[idx]) * (1.0 + jitterFraction * u);
    const double ra = double(std::clamp(retryAfterSeconds, 0, maxRetryAfterSeconds)) * 1000.0;
    return int(std::max(base, ra));
}

void RateLimitGate::note(std::int64_t nowMs, int retryAfterSeconds, int maxSeconds) noexcept {
    if (retryAfterSeconds <= 0) return;
    const std::int64_t end = nowMs + std::int64_t(std::min(retryAfterSeconds, maxSeconds)) * 1000;
    until_ = std::max(until_, end);
}
bool RateLimitGate::blocked(std::int64_t nowMs, std::int64_t& remainingMs) const noexcept {
    remainingMs = until_ > nowMs ? until_ - nowMs : 0;
    return remainingMs > 0;
}

}  // namespace cd::community
