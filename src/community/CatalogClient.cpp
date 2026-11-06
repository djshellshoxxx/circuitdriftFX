#include "community/CatalogClient.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <thread>
#include "community/Sha256.h"
#include "core/Json.h"

namespace cd::community {

using core::JsonValue;

// ---- clock / errors -------------------------------------------------------------------------
std::int64_t SystemClock::nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
bool SystemClock::sleepMs(std::int64_t ms, const CancelToken& cancel) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (cancel.cancelled()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return !cancel.cancelled();
}

const char* errorKindName(ErrorKind k) noexcept {
    switch (k) {
        case ErrorKind::None: return "None";
        case ErrorKind::InvalidRequest: return "InvalidRequest";
        case ErrorKind::Unauthorized: return "Unauthorized";
        case ErrorKind::Forbidden: return "Forbidden";
        case ErrorKind::NotFound: return "NotFound";
        case ErrorKind::Conflict: return "Conflict";
        case ErrorKind::TooLarge: return "TooLarge";
        case ErrorKind::RateLimited: return "RateLimited";
        case ErrorKind::Unavailable: return "Unavailable";
        case ErrorKind::Timeout: return "Timeout";
        case ErrorKind::Cancelled: return "Cancelled";
        case ErrorKind::Network: return "Network";
        case ErrorKind::MalformedResponse: return "MalformedResponse";
        case ErrorKind::DigestMismatch: return "DigestMismatch";
        case ErrorKind::InvalidScene: return "InvalidScene";
        case ErrorKind::ConfirmRequired: return "ConfirmRequired";
        case ErrorKind::Offline: return "Offline";
    }
    return "?";
}

// ---- idempotency journal --------------------------------------------------------------------
std::string IdempotencyJournal::keyFor(const std::string& opId, Rng& rng) {
    std::lock_guard<std::mutex> lock(m_);
    auto it = keys_.find(opId);
    if (it != keys_.end()) return it->second;
    static constexpr char hex[] = "0123456789abcdef";
    std::string k = "ik-";
    for (int i = 0; i < 4; ++i) {
        const std::uint32_t v = rng.next();
        for (int s = 28; s >= 0; s -= 4) k.push_back(hex[(v >> s) & 15]);
    }
    keys_.emplace(opId, k);
    return k;
}
void IdempotencyJournal::forget(const std::string& opId) {
    std::lock_guard<std::mutex> lock(m_);
    keys_.erase(opId);
}
std::map<std::string, std::string> IdempotencyJournal::snapshot() const {
    std::lock_guard<std::mutex> lock(m_);
    return keys_;
}
void IdempotencyJournal::restore(std::map<std::string, std::string> s) {
    std::lock_guard<std::mutex> lock(m_);
    keys_ = std::move(s);
}

// ---- helpers --------------------------------------------------------------------------------
namespace {

ApiError makeError(ErrorKind k, std::string msg, int status = 0) {
    ApiError e;
    e.kind = k;
    e.httpStatus = status;
    e.message = std::move(msg);
    return e;
}

bool validId(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-'))
            return false;
    return true;
}

bool printable(const std::string& s, std::size_t maxLen, bool allowEmpty) {
    if ((s.empty() && !allowEmpty) || s.size() > maxLen) return false;
    for (unsigned char c : s) if (c < 0x20 || c == 0x7f) return false;
    return true;
}

bool validHttpsUrl(const std::string& u) {
    static const std::string scheme = "https://";
    if (u.size() <= scheme.size() || u.size() > 2048 || u.compare(0, scheme.size(), scheme) != 0) return false;
    for (unsigned char c : u) if (c <= 0x20 || c == 0x7f || c == '\\') return false;
    const auto end = u.find_first_of("/?#", scheme.size());
    const std::string authority = u.substr(scheme.size(), end == std::string::npos ? std::string::npos : end - scheme.size());
    return !authority.empty() && authority.find('@') == std::string::npos;  // no embedded credentials
}

const JsonValue* field(const JsonValue& o, const char* key, JsonValue::Type t) {
    const JsonValue* v = o.get(key);
    return (v && v->type == t) ? v : nullptr;
}
bool getString(const JsonValue& o, const char* key, std::size_t maxLen, std::string& out, bool allowEmpty = false) {
    const JsonValue* v = field(o, key, JsonValue::Type::String);
    if (!v || !printable(v->string, maxLen, allowEmpty)) return false;
    out = v->string;
    return true;
}
bool getInt(const JsonValue& o, const char* key, std::int64_t lo, std::int64_t hi, std::int64_t& out) {
    const JsonValue* v = field(o, key, JsonValue::Type::Number);
    if (!v || !v->numberIsExactInteger || v->number < double(lo) || v->number > double(hi)) return false;
    out = static_cast<std::int64_t>(v->number);
    return true;
}

// Bounded strict parse + envelope check (requestId + schemaVersion == 1).
bool parseEnvelope(const std::string& body, std::size_t maxBytes, JsonValue& out, std::string& why) {
    if (body.size() > maxBytes) { why = "response exceeds size cap"; return false; }
    core::JsonLimits lim;
    lim.maxBytes = maxBytes;
    lim.maxDepth = 20;
    if (!core::parseJson(body, out, why, lim)) return false;
    if (!out.isObject()) { why = "response is not an object"; return false; }
    std::string rid;
    std::int64_t sv = 0;
    if (!getString(out, "requestId", 128, rid)) { why = "missing requestId"; return false; }
    if (!getInt(out, "schemaVersion", 1, 1, sv)) { why = "missing or unsupported schemaVersion"; return false; }
    return true;
}

std::string headerValue(const Headers& h, const char* name) {
    for (const auto& [k, v] : h) {
        if (k.size() != std::char_traits<char>::length(name)) continue;
        bool eq = true;
        for (std::size_t i = 0; i < k.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(k[i])) != std::tolower(static_cast<unsigned char>(name[i]))) { eq = false; break; }
        if (eq) return v;
    }
    return {};
}

ErrorKind kindForStatus(int s) {
    switch (s) {
        case 400: return ErrorKind::InvalidRequest;
        case 401: return ErrorKind::Unauthorized;
        case 403: return ErrorKind::Forbidden;
        case 404: return ErrorKind::NotFound;
        case 409: return ErrorKind::Conflict;
        case 413: return ErrorKind::TooLarge;
        case 429: return ErrorKind::RateLimited;
        case 503: return ErrorKind::Unavailable;
        default: break;
    }
    if (s >= 500 && s < 600) return ErrorKind::Unavailable;
    if (s >= 400 && s < 500) return ErrorKind::InvalidRequest;
    return ErrorKind::MalformedResponse;  // 1xx/3xx or nonsense: never followed or trusted
}

std::int64_t parseRetryAfterSeconds(const std::string& s) {
    if (s.empty() || s.size() > 7) return -1;
    std::int64_t v = 0;
    for (char c : s) { if (c < '0' || c > '9') return -1; v = v * 10 + (c - '0'); }
    return v * 1000;
}

ApiError errorFromResponse(const TransportResponse& r) {
    ApiError e = makeError(kindForStatus(r.status), "", r.status);
    e.retryAfterMs = parseRetryAfterSeconds(headerValue(r.headers, "Retry-After"));
    JsonValue v;
    std::string why;
    if (r.body.size() <= kMetadataResponseMaxBytes && core::parseJson(r.body, v, why, {kMetadataResponseMaxBytes, 20}) &&
        v.isObject()) {
        getString(v, "requestId", 128, e.requestId);
        getString(v, "code", 64, e.code);
        getString(v, "message", 256, e.message, true);
        const JsonValue* ra = field(v, "retryAfter", JsonValue::Type::Number);
        if (ra && std::isfinite(ra->number) && ra->number >= 0 && ra->number <= 86400)
            e.retryAfterMs = std::max<std::int64_t>(e.retryAfterMs, static_cast<std::int64_t>(ra->number * 1000.0));
    }
    return e;
}

const char* methodName(HttpMethod m) {
    switch (m) {
        case HttpMethod::Get: return "GET";
        case HttpMethod::Post: return "POST";
        case HttpMethod::Patch: return "PATCH";
        case HttpMethod::Delete: return "DELETE";
    }
    return "?";
}
const char* reasonName(ReportReason r) {
    switch (r) {
        case ReportReason::Copyright: return "copyright";
        case ReportReason::Malicious: return "malicious";
        case ReportReason::Abuse: return "abuse";
        case ReportReason::Spam: return "spam";
        case ReportReason::Other: return "other";
    }
    return "other";
}

bool validMeta(const DraftMetadata& m, std::string& why) {
    if (!printable(m.title, 200, false)) { why = "title must be 1..200 printable chars"; return false; }
    if (m.description.size() > 4000) { why = "description too long"; return false; }
    if (!printable(m.license, 64, false)) { why = "license is required (explicit user choice)"; return false; }
    if (m.tags.size() > 16) { why = "too many tags"; return false; }
    for (const auto& t : m.tags) if (!printable(t, 32, false)) { why = "bad tag"; return false; }
    return true;
}

JsonValue tagsArray(const std::vector<std::string>& tags) {
    JsonValue a = JsonValue::makeArray();
    for (const auto& t : tags) a.array.push_back(JsonValue::makeString(t));
    return a;
}

void addMeta(JsonValue& body, const DraftMetadata& m) {
    body.set("title", JsonValue::makeString(m.title));
    body.set("description", JsonValue::makeString(m.description));
    body.set("tags", tagsArray(m.tags));
    body.set("license", JsonValue::makeString(m.license));
}

bool addScene(JsonValue& body, const state::StateDocument& scene, std::string& why) {
    std::string text;
    if (!state::publishScene(scene, text, why)) return false;  // client-side validation
    JsonValue tree;
    if (!core::parseJson(text, tree, why, {state::kSceneMaxBytes, 20})) return false;
    body.set("scene", std::move(tree));
    return true;
}

}  // namespace

// ---- client core ----------------------------------------------------------------------------
struct CatalogClient::Call {
    HttpMethod method = HttpMethod::Get;
    std::string target;
    Headers query, extraHeaders;
    std::string body;
    bool readOnly = true;
    bool needAuth = false;      // login required (token sent and required)
    std::string idemKey;        // non-empty => write may be retried
    std::size_t maxBytes = kMetadataResponseMaxBytes;
};

CatalogClient::CatalogClient(ClientConfig cfg) : cfg_(std::move(cfg)) {
    if (!cfg_.journal) cfg_.journal = &ownJournal_;
}

void CatalogClient::setAuthToken(AuthToken t) { std::lock_guard<std::mutex> l(m_); token_ = std::move(t); }
void CatalogClient::clearAuthToken() { std::lock_guard<std::mutex> l(m_); token_ = AuthToken(); }
void CatalogClient::setOffline(bool o) noexcept { std::lock_guard<std::mutex> l(m_); offline_ = o; }
int CatalogClient::maxObservedConcurrency() const { std::lock_guard<std::mutex> l(m_); return maxInFlight_; }

void CatalogClient::logLine(const std::string& s) const { if (cfg_.log) cfg_.log(s); }

std::string CatalogClient::idempotencyKey(const std::string& opId) { std::lock_guard<std::mutex> rl(rngMutex_); return cfg_.journal->keyFor(opId, *cfg_.rng); }

bool CatalogClient::acquire(const CancelToken& cancel) {
    std::unique_lock<std::mutex> l(m_);
    while (inFlight_ >= kMaxConcurrentRequests) {
        if (cancel.cancelled()) return false;
        cv_.wait_for(l, std::chrono::milliseconds(5));
    }
    if (cancel.cancelled()) return false;
    ++inFlight_;
    maxInFlight_ = std::max(maxInFlight_, inFlight_);
    return true;
}
std::size_t CatalogClient::cachedMetadataEntries() const {
    std::lock_guard<std::mutex> l(m_);
    return listCache_.size() + revisionCache_.size();
}
void CatalogClient::release() {
    { std::lock_guard<std::mutex> l(m_); --inFlight_; }
    cv_.notify_one();
}

namespace {
// Presigned URLs carry credentials in the query/fragment: never log past '?' or '#'.
std::string logTarget(const std::string& t) { return t.substr(0, t.find_first_of("?#")); }
}  // namespace

CatalogClient::Outcome CatalogClient::run(const Call& c, const CancelToken& cancel) {
    Outcome out;
    std::string authHeader;
    {
        std::lock_guard<std::mutex> l(m_);
        if (offline_) { out.error = makeError(ErrorKind::Offline, "offline mode"); return out; }
        if (c.needAuth) {
            if (token_.empty()) { out.error = makeError(ErrorKind::Unauthorized, "login required"); return out; }
            authHeader = token_.headerValue();
        }
    }
    const bool retriesAllowed = c.readOnly || !c.idemKey.empty();
    static const std::int64_t kBackoffMs[kMaxReadRetries] = {1000, 2000, 4000};
    for (int attempt = 0;; ++attempt) {
        if (cancel.cancelled()) { out.error = makeError(ErrorKind::Cancelled, "cancelled"); return out; }
        if (!acquire(cancel)) { out.error = makeError(ErrorKind::Cancelled, "cancelled"); return out; }
        TransportRequest req;
        req.method = c.method;
        req.target = c.target;
        req.query = c.query;
        req.headers = c.extraHeaders;
        req.headers.emplace_back("Accept", "application/json");
        if (!c.body.empty()) req.headers.emplace_back("Content-Type", "application/json");
        if (!c.idemKey.empty()) req.headers.emplace_back("Idempotency-Key", c.idemKey);
        if (!authHeader.empty()) req.headers.emplace_back("Authorization", authHeader);
        req.body = c.body;
        req.connectTimeoutMs = kConnectTimeoutMs;
        req.totalTimeoutMs = kTotalTimeoutMs;
        req.maxResponseBytes = c.maxBytes;
        TransportResponse resp;
        {
            struct Slot { CatalogClient* self; ~Slot() { self->release(); } } slot{this};  // released even if send() throws
            resp = cfg_.transport->send(req, cancel);
        }
        out.attempts = attempt + 1;

        bool retryable = false;
        ApiError err;
        switch (resp.outcome) {
            case TransportOutcome::Cancelled: err = makeError(ErrorKind::Cancelled, "cancelled"); break;
            case TransportOutcome::Timeout: err = makeError(ErrorKind::Timeout, "request timed out"); retryable = true; break;
            case TransportOutcome::NetworkError: err = makeError(ErrorKind::Network, "network error"); retryable = true; break;
            case TransportOutcome::TooLarge: err = makeError(ErrorKind::TooLarge, "response too large"); break;
            case TransportOutcome::Completed:
                if (resp.status >= 200 && resp.status < 300) {
                    if (resp.body.size() > c.maxBytes) err = makeError(ErrorKind::TooLarge, "response too large", resp.status);
                    else { out.error = ApiError{}; out.response = std::move(resp); logLine(std::string(methodName(c.method)) + " " + logTarget(c.target) + " -> ok"); return out; }
                } else {
                    err = errorFromResponse(resp);
                    retryable = err.kind == ErrorKind::RateLimited || err.kind == ErrorKind::Unavailable;
                }
                break;
        }
        logLine(std::string(methodName(c.method)) + " " + logTarget(c.target) + " attempt " + std::to_string(attempt + 1) +
                " -> " + errorKindName(err.kind));
        out.error = err;
        if (!retryable || !retriesAllowed || attempt >= kMaxReadRetries) return out;
        if (err.retryAfterMs > kMaxRetryAfterMs) return out;  // surfaced to the UI, not slept on
        std::int64_t delay = kBackoffMs[attempt];
        if (cfg_.maxJitterMs > 0) { std::lock_guard<std::mutex> rl(rngMutex_); delay += static_cast<std::int64_t>(cfg_.rng->next() % std::uint32_t(cfg_.maxJitterMs + 1)); }
        delay = std::max(delay, err.retryAfterMs);
        if (!cfg_.clock->sleepMs(delay, cancel)) { out.error = makeError(ErrorKind::Cancelled, "cancelled"); return out; }
    }
}

namespace {
template <class T>
Result<T> fail(ApiError e, int attempts = 0) {
    Result<T> r;
    r.error = std::move(e);
    r.attempts = attempts;
    return r;
}
template <class T>
Result<T> failKind(ErrorKind k, std::string msg) { return fail<T>(makeError(k, std::move(msg))); }
}  // namespace

// ---- reads ----------------------------------------------------------------------------------
Result<ListPage> CatalogClient::listPresets(const ListQuery& q, const CancelToken& cancel) {
    if (q.limit < 1 || q.limit > kMaxPageSize) return failKind<ListPage>(ErrorKind::InvalidRequest, "limit must be 1..50");
    if (!printable(q.q, 256, true) || !printable(q.cursor, 1024, true) || !printable(q.engineVersion, 64, true))
        return failKind<ListPage>(ErrorKind::InvalidRequest, "bad query text");
    std::string tags;
    for (const auto& t : q.tags) {
        if (!printable(t, 32, false) || t.find(',') != std::string::npos) return failKind<ListPage>(ErrorKind::InvalidRequest, "bad tag");
        tags += (tags.empty() ? "" : ",") + t;
    }
    Call c;
    c.target = "/v1/presets";
    if (!q.q.empty()) c.query.emplace_back("q", q.q);
    if (!q.cursor.empty()) c.query.emplace_back("cursor", q.cursor);
    c.query.emplace_back("limit", std::to_string(q.limit));
    if (!tags.empty()) c.query.emplace_back("tags", tags);
    if (!q.engineVersion.empty()) c.query.emplace_back("engineVersion", q.engineVersion);
    const std::string cacheKey = q.q + "\x1f" + q.cursor + "\x1f" + tags + "\x1f" + q.engineVersion + "\x1f" + std::to_string(q.limit);

    auto cached = [&]() -> std::optional<ListPage> {
        std::lock_guard<std::mutex> l(m_);
        const ListPage* hit = listCache_.get(cacheKey);
        if (!hit) return std::nullopt;
        ListPage p = *hit;
        p.fromCache = true;
        return p;
    };
    Outcome o = run(c, cancel);
    if (!o.error.ok()) {
        const ErrorKind k = o.error.kind;
        if (k == ErrorKind::Offline || k == ErrorKind::Timeout || k == ErrorKind::Network ||
            k == ErrorKind::Unavailable || k == ErrorKind::RateLimited) {
            if (auto p = cached()) { Result<ListPage> r; r.value = std::move(*p); r.attempts = o.attempts; return r; }
        }
        return fail<ListPage>(o.error, o.attempts);
    }
    JsonValue v;
    std::string why;
    if (!parseEnvelope(o.response.body, kMetadataResponseMaxBytes, v, why))
        return fail<ListPage>(makeError(ErrorKind::MalformedResponse, why), o.attempts);
    ListPage page;
    getString(v, "requestId", 128, page.requestId);
    page.etag = headerValue(o.response.headers, "ETag");
    if (page.etag.size() > 256) page.etag.clear();
    const JsonValue* items = field(v, "items", JsonValue::Type::Array);
    if (!items || items->array.size() > std::size_t(q.limit))
        return fail<ListPage>(makeError(ErrorKind::MalformedResponse, "bad items array"), o.attempts);
    for (const auto& it : items->array) {
        PresetSummary s;
        const JsonValue* tags2 = it.isObject() ? field(it, "tags", JsonValue::Type::Array) : nullptr;
        if (!it.isObject() || !getString(it, "id", 64, s.id) || !validId(s.id) || !getString(it, "title", 200, s.title) ||
            !getString(it, "author", 128, s.author, true) || !getInt(it, "latestRevision", 1, 1000000000, s.latestRevision) ||
            !tags2 || tags2->array.size() > 16)
            return fail<ListPage>(makeError(ErrorKind::MalformedResponse, "bad item"), o.attempts);
        for (const auto& t : tags2->array) {
            if (!t.isString() || !printable(t.string, 32, false))
                return fail<ListPage>(makeError(ErrorKind::MalformedResponse, "bad tag"), o.attempts);
            s.tags.push_back(t.string);
        }
        page.items.push_back(std::move(s));
    }
    const JsonValue* nc = v.get("nextCursor");
    if (nc && !nc->isNull()) {
        if (!nc->isString() || !printable(nc->string, 1024, true))
            return fail<ListPage>(makeError(ErrorKind::MalformedResponse, "bad nextCursor"), o.attempts);
        page.nextCursor = nc->string;
    }
    { std::lock_guard<std::mutex> l(m_); listCache_.put(cacheKey, page); }
    Result<ListPage> r;
    r.value = std::move(page);
    r.attempts = o.attempts;
    return r;
}

Result<RevisionInfo> CatalogClient::getRevision(const std::string& id, std::int64_t rev, const CancelToken& cancel) {
    if (!validId(id) || rev < 1) return failKind<RevisionInfo>(ErrorKind::InvalidRequest, "bad id or revision");
    const std::string key = id + "#" + std::to_string(rev);
    Call c;
    c.target = "/v1/presets/" + id + "/revisions/" + std::to_string(rev);
    Outcome o = run(c, cancel);
    if (!o.error.ok()) {
        if (o.error.kind == ErrorKind::Offline || o.error.kind == ErrorKind::Timeout || o.error.kind == ErrorKind::Network ||
            o.error.kind == ErrorKind::Unavailable) {
            std::lock_guard<std::mutex> l(m_);
            if (const RevisionInfo* hit = revisionCache_.get(key)) { Result<RevisionInfo> r; r.value = *hit; r.attempts = o.attempts; return r; }
        }
        return fail<RevisionInfo>(o.error, o.attempts);
    }
    JsonValue v;
    std::string why;
    if (!parseEnvelope(o.response.body, kMetadataResponseMaxBytes, v, why))
        return fail<RevisionInfo>(makeError(ErrorKind::MalformedResponse, why), o.attempts);
    RevisionInfo info;
    std::int64_t revBack = 0;
    if (!getString(v, "presetId", 64, info.presetId) || info.presetId != id || !getInt(v, "revision", 1, 1000000000, revBack) ||
        revBack != rev || !getString(v, "title", 200, info.title) || !getString(v, "blobSha256", 64, info.blobSha256) ||
        !isSha256Hex(info.blobSha256) || !getInt(v, "byteLength", 1, std::int64_t(kSceneBlobMaxBytes), info.byteLength) ||
        !getString(v, "downloadUrl", 2048, info.downloadUrl) || !validHttpsUrl(info.downloadUrl) ||
        !getString(v, "license", 64, info.license, true))
        return fail<RevisionInfo>(makeError(ErrorKind::MalformedResponse, "bad revision metadata"), o.attempts);
    info.revision = rev;
    { std::lock_guard<std::mutex> l(m_); revisionCache_.put(key, info); }
    Result<RevisionInfo> r;
    r.value = std::move(info);
    r.attempts = o.attempts;
    return r;
}

Result<SceneDownload> CatalogClient::fetchScene(const RevisionInfo& info, const CancelToken& cancel, bool pin) {
    if (!isSha256Hex(info.blobSha256) || info.byteLength < 1 || info.byteLength > std::int64_t(kSceneBlobMaxBytes) ||
        !validHttpsUrl(info.downloadUrl))
        return failKind<SceneDownload>(ErrorKind::InvalidRequest, "bad revision info");

    auto parseScene = [&](std::string bytes, bool fromCache, int attempts) -> Result<SceneDownload> {
        SceneDownload d;
        std::string why;
        if (!state::loadState(bytes, d.doc, why) || d.doc.kind != state::DocKind::Scene) {
            if (fromCache && cfg_.cache) cfg_.cache->erase(info.blobSha256);
            return fail<SceneDownload>(makeError(ErrorKind::InvalidScene, why.empty() ? "not a scene document" : why), attempts);
        }
        d.sha256 = info.blobSha256;
        d.bytes = std::move(bytes);
        d.fromCache = fromCache;
        Result<SceneDownload> r;
        r.value = std::move(d);
        r.attempts = attempts;
        return r;
    };

    if (cfg_.cache) {
        if (auto hit = cfg_.cache->get(info.blobSha256)) {  // digest re-verified inside get()
            if (pin) cfg_.cache->setPinned(info.blobSha256, true);
            return parseScene(std::move(*hit), true, 0);
        }
    }
    Call c;
    c.target = info.downloadUrl;  // public blob: no Authorization header is attached
    c.maxBytes = std::size_t(info.byteLength);
    Outcome o = run(c, cancel);
    if (!o.error.ok()) return fail<SceneDownload>(o.error, o.attempts);
    const std::string& body = o.response.body;
    // Digest over the exact downloaded bytes BEFORE any parse.
    if (body.size() != std::size_t(info.byteLength) || sha256Hex(body) != info.blobSha256)
        return fail<SceneDownload>(makeError(ErrorKind::DigestMismatch, "blob digest or length mismatch"), o.attempts);
    Result<SceneDownload> r = parseScene(std::move(o.response.body), false, o.attempts);
    if (r.ok() && cfg_.cache) cfg_.cache->put(info.blobSha256, r.value.bytes, pin);
    return r;
}

// ---- writes ---------------------------------------------------------------------------------
namespace {
bool terminal(ErrorKind k) {
    switch (k) {
        case ErrorKind::None: case ErrorKind::InvalidRequest: case ErrorKind::Unauthorized: case ErrorKind::Forbidden:
        case ErrorKind::NotFound: case ErrorKind::Conflict: case ErrorKind::TooLarge:
            return true;
        default: return false;  // ambiguous outcomes keep the key so a re-submit dedupes
    }
}
}  // namespace

Result<PresetRef> CatalogClient::createDraft(const DraftMetadata& meta, const state::StateDocument& scene,
                                             const std::string& opId, const CancelToken& cancel) {
    std::string why;
    if (!printable(opId, 128, false)) return failKind<PresetRef>(ErrorKind::InvalidRequest, "bad opId");
    if (!validMeta(meta, why)) return failKind<PresetRef>(ErrorKind::InvalidRequest, why);
    JsonValue body = JsonValue::makeObject();
    addMeta(body, meta);
    if (!addScene(body, scene, why)) return failKind<PresetRef>(ErrorKind::InvalidScene, why);
    Call c;
    c.method = HttpMethod::Post;
    c.target = "/v1/presets";
    c.readOnly = false;
    c.needAuth = true;
    c.idemKey = idempotencyKey(opId);
    body.set("idempotencyKey", JsonValue::makeString(c.idemKey));
    c.body = core::writeJson(body);
    Outcome o = run(c, cancel);
    Result<PresetRef> r;
    r.attempts = o.attempts;
    if (!o.error.ok()) { r.error = o.error; }
    else {
        JsonValue v;
        std::int64_t rev = 0;
        if (!parseEnvelope(o.response.body, kMetadataResponseMaxBytes, v, why) || !getString(v, "id", 64, r.value.id) ||
            !validId(r.value.id) || !getInt(v, "revision", 1, 1000000000, rev))
            r.error = makeError(ErrorKind::MalformedResponse, why.empty() ? "bad draft response" : why);
        r.value.revision = rev;
    }
    if (terminal(r.error.kind)) cfg_.journal->forget(opId);
    return r;
}

namespace {
// Shared parse for {id, revision} acknowledgements that must echo the requested id.
ApiError parseRef(const std::string& body, const std::string& id, PresetRef& out) {
    JsonValue v;
    std::string why;
    std::int64_t rev = 0;
    if (!parseEnvelope(body, kMetadataResponseMaxBytes, v, why)) return makeError(ErrorKind::MalformedResponse, why);
    if (!getString(v, "id", 64, out.id) || out.id != id || !getInt(v, "revision", 1, 1000000000, rev))
        return makeError(ErrorKind::MalformedResponse, "bad acknowledgement");
    out.revision = rev;
    return {};
}
}  // namespace

Result<PresetRef> CatalogClient::publish(const std::string& id, std::int64_t expectedRevision, bool confirmed,
                                         const std::string& opId, const CancelToken& cancel) {
    if (!confirmed) return failKind<PresetRef>(ErrorKind::ConfirmRequired, "publish requires explicit confirmation");
    if (!validId(id) || expectedRevision < 1 || !printable(opId, 128, false))
        return failKind<PresetRef>(ErrorKind::InvalidRequest, "bad id, revision or opId");
    JsonValue body = JsonValue::makeObject();
    body.set("expectedRevision", JsonValue::makeNumber(double(expectedRevision)));
    body.set("visibility", JsonValue::makeString("public"));
    Call c;
    c.method = HttpMethod::Post;
    c.target = "/v1/presets/" + id + "/publish";
    c.readOnly = false;
    c.needAuth = true;
    c.idemKey = idempotencyKey(opId);
    body.set("idempotencyKey", JsonValue::makeString(c.idemKey));
    c.body = core::writeJson(body);
    Outcome o = run(c, cancel);
    Result<PresetRef> r;
    r.attempts = o.attempts;
    r.error = o.error.ok() ? parseRef(o.response.body, id, r.value) : o.error;
    if (terminal(r.error.kind)) cfg_.journal->forget(opId);
    return r;
}

Result<PresetRef> CatalogClient::patch(const std::string& id, std::int64_t ifMatchRevision, const DraftMetadata& meta,
                                       const state::StateDocument* newScene, const std::string& opId,
                                       const CancelToken& cancel) {
    std::string why;
    if (!validId(id) || ifMatchRevision < 1 || !printable(opId, 128, false))
        return failKind<PresetRef>(ErrorKind::InvalidRequest, "bad id, revision or opId");
    if (!validMeta(meta, why)) return failKind<PresetRef>(ErrorKind::InvalidRequest, why);
    JsonValue body = JsonValue::makeObject();
    addMeta(body, meta);
    if (newScene && !addScene(body, *newScene, why)) return failKind<PresetRef>(ErrorKind::InvalidScene, why);
    Call c;
    c.method = HttpMethod::Patch;
    c.target = "/v1/presets/" + id;
    c.readOnly = false;
    c.needAuth = true;
    c.idemKey = idempotencyKey(opId);
    c.extraHeaders.emplace_back("If-Match", std::to_string(ifMatchRevision));
    body.set("idempotencyKey", JsonValue::makeString(c.idemKey));
    c.body = core::writeJson(body);
    Outcome o = run(c, cancel);
    Result<PresetRef> r;
    r.attempts = o.attempts;
    r.error = o.error.ok() ? parseRef(o.response.body, id, r.value) : o.error;
    if (terminal(r.error.kind)) cfg_.journal->forget(opId);
    return r;
}

Result<PresetRef> CatalogClient::remove(const std::string& id, std::int64_t expectedRevision, const std::string& opId,
                                        const CancelToken& cancel) {
    if (!validId(id) || expectedRevision < 1 || !printable(opId, 128, false))
        return failKind<PresetRef>(ErrorKind::InvalidRequest, "bad id, revision or opId");
    Call c;
    c.method = HttpMethod::Delete;
    c.target = "/v1/presets/" + id;
    c.query.emplace_back("expectedRevision", std::to_string(expectedRevision));
    c.readOnly = false;
    c.needAuth = true;
    c.idemKey = idempotencyKey(opId);
    Outcome o = run(c, cancel);  // local cache is intentionally untouched (tombstone is remote only)
    Result<PresetRef> r;
    r.attempts = o.attempts;
    if (!o.error.ok()) r.error = o.error;
    else {
        r.error = parseRef(o.response.body, id, r.value);
        JsonValue v;
        std::string why;
        if (r.error.ok() && (!parseEnvelope(o.response.body, kMetadataResponseMaxBytes, v, why) ||
                             !field(v, "tombstone", JsonValue::Type::Bool) || !v.get("tombstone")->boolean))
            r.error = makeError(ErrorKind::MalformedResponse, "missing tombstone acknowledgement");
    }
    if (terminal(r.error.kind)) cfg_.journal->forget(opId);
    return r;
}

Result<std::string> CatalogClient::report(const std::string& id, std::int64_t rev, ReportReason reason,
                                          const std::string& text, const std::string& opId, const CancelToken& cancel) {
    if (!validId(id) || rev < 1 || !printable(opId, 128, false) || text.size() > 2000)
        return failKind<std::string>(ErrorKind::InvalidRequest, "bad report request");
    JsonValue body = JsonValue::makeObject();
    body.set("presetId", JsonValue::makeString(id));
    body.set("revision", JsonValue::makeNumber(double(rev)));
    body.set("reason", JsonValue::makeString(reasonName(reason)));
    if (!text.empty()) body.set("text", JsonValue::makeString(text));
    Call c;
    c.method = HttpMethod::Post;
    c.target = "/v1/reports";
    c.readOnly = false;
    c.needAuth = true;
    c.idemKey = idempotencyKey(opId);
    body.set("idempotencyKey", JsonValue::makeString(c.idemKey));
    c.body = core::writeJson(body);
    Outcome o = run(c, cancel);
    Result<std::string> r;
    r.attempts = o.attempts;
    if (!o.error.ok()) r.error = o.error;
    else {
        JsonValue v;
        std::string why;
        if (!parseEnvelope(o.response.body, kMetadataResponseMaxBytes, v, why) || !getString(v, "reportId", 64, r.value))
            r.error = makeError(ErrorKind::MalformedResponse, why.empty() ? "bad report response" : why);
    }
    if (terminal(r.error.kind)) cfg_.journal->forget(opId);
    return r;
}

}  // namespace cd::community
