#include "community/CommunityClient.h"
#include <algorithm>
#include <cmath>
#include "community/Sha256.h"
#include "core/Json.h"

namespace cd::community {
using core::JsonValue;

namespace {
bool noControl(const std::string& s) {
    for (unsigned char c : s) if (c < 0x20 || c == 0x7F) return false;
    return true;
}
bool getStr(const JsonValue& o, const char* key, std::size_t maxLen, std::string& out, bool required = true) {
    const JsonValue* v = o.get(key);
    if (!v) return !required;
    if (!v->isString() || v->string.size() > maxLen || !noControl(v->string)) return false;
    out = v->string;
    return true;
}
bool getInt(const JsonValue& o, const char* key, std::uint64_t lo, std::uint64_t hi, std::uint64_t& out) {
    const JsonValue* v = o.get(key);
    if (!v || !v->isNumber() || !v->numberIsExactInteger || v->number < double(lo) || v->number > double(hi)) return false;
    out = std::uint64_t(v->number);
    return true;
}
bool getStrArray(const JsonValue& o, const char* key, std::size_t maxItems, std::size_t maxLen, std::vector<std::string>& out) {
    const JsonValue* v = o.get(key);
    if (!v) return true;
    if (!v->isArray() || v->array.size() > maxItems) return false;
    for (const auto& e : v->array) {
        if (!e.isString() || e.string.size() > maxLen || !noControl(e.string)) return false;
        out.push_back(e.string);
    }
    return true;
}
// Envelope: object with schemaVersion == 1 and optional requestId.
bool parseEnvelope(const std::string& body, std::size_t cap, JsonValue& v, std::string& requestId) {
    core::JsonLimits lim; lim.maxBytes = cap; lim.maxDepth = 20;
    std::string err;
    if (body.empty() || !core::parseJson(body, v, err, lim) || !v.isObject()) return false;
    const JsonValue* sv = v.get("schemaVersion");
    if (!sv || !sv->isNumber() || !sv->numberIsExactInteger || sv->number != 1) return false;
    if (const JsonValue* r = v.get("requestId")) { if (!r->isString() || r->string.size() > 64 || !noControl(r->string)) return false; requestId = r->string; }
    return true;
}
bool validRevisionId(const std::string& s) { return isSafeIdentifier(s); }

const char* reasonText(ReportReason r) {
    switch (r) {
        case ReportReason::Spam: return "spam"; case ReportReason::Offensive: return "offensive";
        case ReportReason::Copyright: return "copyright"; case ReportReason::Malicious: return "malicious";
        case ReportReason::Other: return "other";
    }
    return "other";
}
JsonValue tagsJson(const std::vector<std::string>& tags) {
    JsonValue a = JsonValue::makeArray();
    for (const auto& t : tags) a.array.push_back(JsonValue::makeString(t));
    return a;
}
}  // namespace

bool isSafeUrl(const std::string& url) {
    static const std::string kScheme = "https://";
    if (url.size() <= kScheme.size() || url.size() > 2048 || url.compare(0, kScheme.size(), kScheme) != 0) return false;
    for (unsigned char c : url) if (c <= 0x20 || c >= 0x7F) return false;
    const std::size_t end = url.find_first_of("/?#", kScheme.size());
    const std::string authority = url.substr(kScheme.size(), end == std::string::npos ? std::string::npos : end - kScheme.size());
    return !authority.empty() && authority.find('@') == std::string::npos && authority[0] != ':';
}

std::string makeIdempotencyKey(const std::function<std::uint64_t()>& rng) {
    std::string s;
    for (int i = 0; i < 2; ++i) {
        std::uint8_t b[8]; const std::uint64_t r = rng();
        for (int k = 0; k < 8; ++k) b[k] = std::uint8_t(r >> (8 * k));
        s += toHex(b, 8);
    }
    return s;
}
bool isValidIdempotencyKey(const std::string& k) {
    if (k.size() < 16 || k.size() > 64) return false;
    return isSafeIdentifier(k);
}

CommunityClient::CommunityClient(Transport& t, TokenProvider* tokens, OfflineCache* cache, ClientConfig cfg, ClientEnv env)
    : transport_(t), tokens_(tokens), cache_(cache), cfg_(std::move(cfg)), env_(std::move(env)) {}

Result CommunityClient::fail(Status s, std::string msg) const {
    Result r; r.status = s; r.message = std::move(msg);
    return r;
}

Result CommunityClient::execute(Call call, HttpResponse& resp) {
    std::string token;
    if (call.auth) {
        if (!tokens_ || !tokens_->accessToken(token, false) || token.empty()) return fail(Status::AuthRequired, "sign-in required");
    }
    struct Slot {
        std::atomic<int>& n; bool ok;
        explicit Slot(std::atomic<int>& a) : n(a), ok(a.fetch_add(1) < 2) {}
        ~Slot() { n.fetch_sub(1); }
    } slot(inFlight_);
    if (!slot.ok) return fail(Status::Busy, "too many concurrent requests");
    bool refreshed = false;
    for (int attempt = 0;;) {
        if (call.req.cancel && call.req.cancel->load()) return fail(Status::Cancelled, "cancelled");
        if (attempt == 0) {
            std::int64_t remaining = 0;
            if (gate_.blocked(now(), remaining)) {
                Result r = fail(Status::RateLimited, "waiting for the server rate limit to clear");
                r.retryAfterSeconds = int((remaining + 999) / 1000);
                return r;
            }
        }
        HttpRequest r = call.req;
        r.headers.emplace_back("Accept", "application/json");
        if (call.auth) r.headers.emplace_back("Authorization", "Bearer " + token);
        ++requests_;
        resp = transport_.send(r);
        if (resp.body.size() > r.maxResponseBytes) return fail(Status::ResponseTooLarge, "response exceeds the size cap");
        Result res = mapHttpResponse(resp, token);
        if (res.status == Status::Unauthorized && call.auth && !refreshed) {
            refreshed = true;
            if (tokens_ && tokens_->accessToken(token, true) && !token.empty()) continue;
        }
        if (res.status == Status::RateLimited || res.status == Status::ServiceUnavailable) gate_.note(now(), res.retryAfterSeconds, cfg_.retry.maxRetryAfterSeconds);
        if (call.retryable && cfg_.retry.shouldRetry(res.status, attempt, res.retryAfterSeconds)) {
            if (env_.sleep) env_.sleep(cfg_.retry.delayMs(attempt, res.retryAfterSeconds, env_.random01 ? env_.random01() : 0.0));
            ++attempt;
            continue;
        }
        return res;
    }
}

Result CommunityClient::browse(const BrowseQuery& q, BrowsePage& out) {
    if (q.limit < 1 || q.limit > kMaxPageItems || q.q.size() > 256 || q.cursor.size() > 512 || q.engineVersion.size() > 64 || q.tags.size() > 16 ||
        !noControl(q.q) || !noControl(q.cursor) || !noControl(q.ifNoneMatch) || q.ifNoneMatch.size() > 256)
        return fail(Status::InvalidArgument, "invalid browse query");
    Call c; c.retryable = true;
    c.req.method = HttpMethod::Get; c.req.target = "/v1/presets"; c.req.maxResponseBytes = kMaxJsonResponseBytes;
    if (!q.q.empty()) c.req.query.emplace_back("q", q.q);
    if (!q.cursor.empty()) c.req.query.emplace_back("cursor", q.cursor);
    c.req.query.emplace_back("limit", std::to_string(q.limit));
    if (!q.tags.empty()) { std::string t; for (auto& x : q.tags) { if (!t.empty()) t += ','; t += x; } c.req.query.emplace_back("tags", t); }
    if (!q.engineVersion.empty()) c.req.query.emplace_back("engineVersion", q.engineVersion);
    if (!q.ifNoneMatch.empty()) c.req.headers.emplace_back("If-None-Match", q.ifNoneMatch);
    HttpResponse resp;
    Result r = execute(std::move(c), resp);
    if (r.status == Status::NotModified) return r;
    if (r.status != Status::Ok) return r.ok() ? fail(Status::UnexpectedStatus, "unexpected success status") : r;
    JsonValue v; std::string rid;
    if (!parseEnvelope(resp.body, kMaxJsonResponseBytes, v, rid)) return fail(Status::MalformedResponse, "malformed catalog response");
    const JsonValue* items = v.get("items");
    if (!items || !items->isArray() || items->array.size() > q.limit) return fail(Status::MalformedResponse, "malformed item list");
    BrowsePage page; page.requestId = rid;
    for (const auto& it : items->array) {
        PresetSummary s;
        if (!it.isObject() || !getStr(it, "id", 64, s.id) || !isSafeIdentifier(s.id) || !getStr(it, "title", 120, s.title) ||
            !getStr(it, "author", 80, s.author, false) || !getStr(it, "latestRevision", 64, s.latestRevision) || !validRevisionId(s.latestRevision) ||
            !getStr(it, "engineModelVersion", 64, s.engineModelVersion, false) || !getStrArray(it, "tags", 16, 32, s.tags))
            return fail(Status::MalformedResponse, "malformed catalog item");
        page.items.push_back(std::move(s));
    }
    if (!getStr(v, "nextCursor", 512, page.nextCursor, false)) return fail(Status::MalformedResponse, "malformed cursor");
    if (const std::string* e = findHeader(resp.headers, "ETag")) { if (e->size() > 256 || !noControl(*e)) return fail(Status::MalformedResponse, "malformed ETag"); page.etag = *e; }
    out = std::move(page);
    return r;
}

Result CommunityClient::getRevision(const std::string& id, const std::string& rev, RevisionInfo& out) {
    if (!isSafeIdentifier(id) || !validRevisionId(rev)) return fail(Status::InvalidArgument, "invalid preset identifier");
    Call c; c.retryable = true;
    c.req.method = HttpMethod::Get; c.req.target = "/v1/presets/" + id + "/revisions/" + rev; c.req.maxResponseBytes = kMaxJsonResponseBytes;
    HttpResponse resp;
    Result r = execute(std::move(c), resp);
    if (r.status != Status::Ok) return r.ok() ? fail(Status::UnexpectedStatus, "unexpected success status") : r;
    JsonValue v; std::string rid;
    if (!parseEnvelope(resp.body, kMaxJsonResponseBytes, v, rid)) return fail(Status::MalformedResponse, "malformed revision response");
    const JsonValue* m = v.get("revision");
    if (!m || !m->isObject()) return fail(Status::MalformedResponse, "missing revision metadata");
    RevisionInfo info; info.requestId = rid;
    std::uint64_t reader = 0, len = 0;
    if (!getStr(*m, "id", 64, info.id) || !getStr(*m, "revision", 64, info.revision) || !getStr(*m, "ownerId", 128, info.ownerId) ||
        !getStr(*m, "title", 120, info.title) || !getStr(*m, "description", 4000, info.description, false) || !getStr(*m, "license", 64, info.license) ||
        !getStr(*m, "kind", 16, info.kind) || !getStr(*m, "engineModelVersion", 64, info.engineModelVersion) ||
        !getStr(*m, "blobSha256", 64, info.blobSha256) || !getStr(*m, "downloadUrl", 2048, info.downloadUrl) ||
        !getStr(*m, "visibility", 16, info.visibility, false) || !getStrArray(*m, "requiredFeatures", 32, 64, info.requiredFeatures) ||
        !getInt(*m, "minReaderVersion", 1, 1000, reader) || !getInt(*m, "byteLength", 1, cfg_.maxBlobBytes, len))
        return fail(Status::MalformedResponse, "malformed revision metadata");
    if (info.id != id || info.revision != rev) return fail(Status::MalformedResponse, "revision identity mismatch");
    if (!isSha256Hex(info.blobSha256)) return fail(Status::MalformedResponse, "malformed blob digest");
    if (!isSafeUrl(info.downloadUrl)) return fail(Status::MalformedResponse, "unsafe download URL");
    if (info.kind != "scene") return fail(Status::Incompatible, "only scenes are shared");
    info.minReaderVersion = int(reader); info.byteLength = len;
    out = std::move(info);
    return r;
}

Compatibility CommunityClient::checkCompatibility(const RevisionInfo& i, const LocalCapabilities& caps) {
    Compatibility c;
    if (i.kind != "scene") c.reasons.push_back("not a scene document");
    if (i.engineModelVersion != caps.engineModelVersion) c.reasons.push_back("built for engine model " + sanitizeText(i.engineModelVersion, {}, 64));
    if (i.minReaderVersion > caps.readerVersion) c.reasons.push_back("requires a newer reader");
    for (const auto& f : i.requiredFeatures) if (!caps.features.count(f)) c.reasons.push_back("requires unavailable feature " + sanitizeText(f, {}, 64));
    c.compatible = c.reasons.empty();
    return c;
}

Result CommunityClient::validateSceneBytes(const std::string& bytes, const std::string& expectedSha, std::uint64_t expectedLen,
                                           std::size_t maxBytes, ImportedScene& out) {
    Result r;
    if (bytes.size() > maxBytes) { r.status = Status::ResponseTooLarge; r.message = "scene exceeds the size bound"; return r; }
    if (expectedLen && bytes.size() != expectedLen) { r.status = Status::DigestMismatch; r.message = "downloaded length differs from the revision metadata"; return r; }
    const std::string sha = sha256Hex(bytes);  // exact downloaded bytes, before any parse
    if (!expectedSha.empty() && sha != expectedSha) { r.status = Status::DigestMismatch; r.message = "content digest does not match the revision"; return r; }
    core::JsonValue tree; std::string err;
    core::JsonLimits lim; lim.maxBytes = state::kSceneMaxBytes; lim.maxDepth = 20;
    if (!core::parseJson(bytes, tree, err, lim)) { r.status = Status::MalformedResponse; r.message = "scene is not valid bounded JSON: " + sanitizeText(err, {}, 120); return r; }
    state::StateDocument doc;
    if (!state::loadState(bytes, doc, err)) { r.status = Status::ValidationFailed; r.message = "scene rejected by schema validation: " + sanitizeText(err, {}, 160); return r; }
    std::string canonical;
    if (doc.kind != state::DocKind::Scene || doc.checkpoint || !state::publishScene(doc, canonical, err)) {
        r.status = Status::ValidationFailed;
        r.message = "scene rejected by publication validation: " + sanitizeText(err, {}, 160);
        return r;
    }
    out.doc = std::move(doc); out.bytes = bytes; out.sha256 = sha;
    return r;
}

Result CommunityClient::fetchScene(const std::string& id, const std::string& rev, bool preferCache, ImportedScene& out) {
    if (!isSafeIdentifier(id) || !validRevisionId(rev)) return fail(Status::InvalidArgument, "invalid preset identifier");
    auto fromCache = [&](Result& why) {
        std::string bytes;
        if (!cache_) return false;
        const CacheGet g = cache_->get(id, rev, bytes);
        if (g == CacheGet::Corrupt) { why = fail(Status::CacheCorrupt, "cached copy failed its integrity check"); return false; }
        if (g != CacheGet::Hit) return false;
        ImportedScene tmp;
        Result v = validateSceneBytes(bytes, {}, 0, cfg_.maxBlobBytes, tmp);
        if (!v.ok()) { cache_->remove(id, rev); why = fail(Status::CacheCorrupt, "cached copy failed validation"); return false; }
        tmp.fromCache = true;
        out = std::move(tmp);
        return true;
    };
    Result cacheWhy;
    if (preferCache && fromCache(cacheWhy)) return Result{};
    RevisionInfo info;
    Result r = getRevision(id, rev, info);
    if (!r.ok()) {
        if (r.status == Status::Timeout || r.status == Status::TransportError) {
            if (!preferCache && fromCache(cacheWhy)) return Result{};  // offline fallback
            return fail(Status::OfflineMiss, "offline and no valid cached copy");
        }
        return r;
    }
    Call c; c.retryable = true;
    c.req.method = HttpMethod::Get; c.req.target = info.downloadUrl;  // no Authorization header: blob host != API host
    c.req.maxResponseBytes = std::size_t(info.byteLength);
    HttpResponse resp;
    r = execute(std::move(c), resp);
    if (r.status != Status::Ok) {
        if (r.status == Status::Timeout || r.status == Status::TransportError) {
            if (fromCache(cacheWhy)) return Result{};
            return fail(Status::OfflineMiss, "offline and no valid cached copy");
        }
        return r.ok() ? fail(Status::UnexpectedStatus, "unexpected download status") : r;
    }
    ImportedScene scene;
    Result v = validateSceneBytes(resp.body, info.blobSha256, info.byteLength, cfg_.maxBlobBytes, scene);
    if (!v.ok()) { v.requestId = info.requestId; return v; }
    // The revision metadata must describe the same model as the document.
    if (scene.doc.engineModelVersion != info.engineModelVersion) return fail(Status::ValidationFailed, "metadata and document disagree on engine model");
    if (cache_) { std::string e; cache_->put(id, rev, scene.bytes, e); }  // best effort, never fatal
    out = std::move(scene);
    Result ok; ok.httpStatus = 200; ok.requestId = info.requestId;
    return ok;
}

namespace {
// Shared draft/update body; validates metadata and serialises the scene only via publishScene.
Result buildBody(const PublishRequest& req, bool sceneRequired, JsonValue& body, const CommunityClient&) {
    Result r;
    auto bad = [&](Status s, const char* m) { r.status = s; r.message = m; return r; };
    if (req.title.empty() || req.title.size() > 120 || !noControl(req.title)) return bad(Status::InvalidArgument, "title must be 1..120 characters");
    if (req.description.size() > 4000) return bad(Status::InvalidArgument, "description too long");
    if (req.license.empty() || req.license.size() > 64 || !noControl(req.license)) return bad(Status::InvalidArgument, "license is required");
    if (req.tags.size() > 16) return bad(Status::InvalidArgument, "too many tags");
    for (auto& t : req.tags) if (t.empty() || t.size() > 32 || !noControl(t)) return bad(Status::InvalidArgument, "invalid tag");
    body = JsonValue::makeObject();
    body.set("title", JsonValue::makeString(req.title));
    body.set("description", JsonValue::makeString(req.description));
    body.set("tags", tagsJson(req.tags));
    body.set("license", JsonValue::makeString(req.license));
    if (req.scene || sceneRequired) {
        if (!req.scene) return bad(Status::InvalidArgument, "scene is required");
        std::string text, err;
        if (!state::publishScene(*req.scene, text, err)) { r.status = Status::ValidationFailed; r.message = "scene is not publishable: " + sanitizeText(err, {}, 160); return r; }
        JsonValue tree; core::JsonLimits lim; lim.maxBytes = state::kSceneMaxBytes; lim.maxDepth = 20;
        if (!core::parseJson(text, tree, err, lim)) return bad(Status::ValidationFailed, "scene re-parse failed");
        body.set("scene", std::move(tree));
    }
    return r;
}
}  // namespace

Result CommunityClient::createDraft(const PublishRequest& req, DraftInfo& out) {
    if (!isValidIdempotencyKey(req.idempotencyKey)) return fail(Status::InvalidArgument, "invalid idempotency key");
    JsonValue body;
    Result b = buildBody(req, true, body, *this);
    if (!b.ok()) return b;
    body.set("idempotencyKey", JsonValue::makeString(req.idempotencyKey));
    Call c; c.auth = true; c.retryable = true;  // safe: every attempt carries the same key
    c.req.method = HttpMethod::Post; c.req.target = "/v1/presets"; c.req.maxResponseBytes = kMaxJsonResponseBytes;
    try { c.req.body = core::writeJson(body); } catch (const std::exception&) { return fail(Status::InvalidArgument, "request not serialisable"); }
    if (c.req.body.size() > state::kSceneMaxBytes + 16384) return fail(Status::PayloadTooLarge, "request exceeds the size bound");
    c.req.headers.emplace_back("Content-Type", "application/json");
    c.req.headers.emplace_back("Idempotency-Key", req.idempotencyKey);
    HttpResponse resp;
    Result r = execute(std::move(c), resp);
    if (r.status != Status::Created && r.status != Status::Ok) return r.ok() ? fail(Status::UnexpectedStatus, "unexpected success status") : r;
    JsonValue v; std::string rid;
    DraftInfo d;
    if (!parseEnvelope(resp.body, kMaxJsonResponseBytes, v, rid) || !getStr(v, "id", 64, d.id) || !isSafeIdentifier(d.id) ||
        !getStr(v, "revision", 64, d.revision) || !validRevisionId(d.revision))
        return fail(Status::MalformedResponse, "malformed draft response");
    d.requestId = rid;
    out = std::move(d);
    return r;
}

Result CommunityClient::publish(const std::string& id, const std::string& expectedRevision, bool userConfirmed, const std::string& key, DraftInfo& out) {
    if (!userConfirmed) return fail(Status::PublishNotConfirmed, "publishing requires an explicit confirmation");
    if (!isSafeIdentifier(id) || !validRevisionId(expectedRevision) || !isValidIdempotencyKey(key)) return fail(Status::InvalidArgument, "invalid publish arguments");
    JsonValue body = JsonValue::makeObject();
    body.set("expectedRevision", JsonValue::makeString(expectedRevision));
    body.set("visibility", JsonValue::makeString("public"));
    Call c; c.auth = true; c.retryable = true;
    c.req.method = HttpMethod::Post; c.req.target = "/v1/presets/" + id + "/publish"; c.req.maxResponseBytes = kMaxJsonResponseBytes;
    c.req.body = core::writeJson(body);
    c.req.headers.emplace_back("Content-Type", "application/json");
    c.req.headers.emplace_back("Idempotency-Key", key);
    HttpResponse resp;
    Result r = execute(std::move(c), resp);
    if (r.status != Status::Ok) return r.ok() ? fail(Status::UnexpectedStatus, "unexpected success status") : r;
    JsonValue v; std::string rid; DraftInfo d;
    if (!parseEnvelope(resp.body, kMaxJsonResponseBytes, v, rid) || !getStr(v, "revision", 64, d.revision) || !validRevisionId(d.revision))
        return fail(Status::MalformedResponse, "malformed publish response");
    d.id = id; d.requestId = rid;
    out = std::move(d);
    return r;
}

Result CommunityClient::update(const std::string& id, const RevisionInfo& cur, const PublishRequest& req, DraftInfo& out) {
    if (!isSafeIdentifier(id) || !validRevisionId(cur.revision)) return fail(Status::InvalidArgument, "invalid preset identifier");
    if (cfg_.accountId.empty() || cur.ownerId != cfg_.accountId) return fail(Status::NotOwner, "only the owner can edit this preset");
    JsonValue body;
    Result b = buildBody(req, false, body, *this);
    if (!b.ok()) return b;
    Call c; c.auth = true; c.retryable = false;  // a blind retry after success would conflict
    c.req.method = HttpMethod::Patch; c.req.target = "/v1/presets/" + id; c.req.maxResponseBytes = kMaxJsonResponseBytes;
    try { c.req.body = core::writeJson(body); } catch (const std::exception&) { return fail(Status::InvalidArgument, "request not serialisable"); }
    c.req.headers.emplace_back("Content-Type", "application/json");
    c.req.headers.emplace_back("If-Match", cur.revision);
    HttpResponse resp;
    Result r = execute(std::move(c), resp);
    if (r.status != Status::Ok) return r.ok() ? fail(Status::UnexpectedStatus, "unexpected success status") : r;
    JsonValue v; std::string rid; DraftInfo d;
    if (!parseEnvelope(resp.body, kMaxJsonResponseBytes, v, rid) || !getStr(v, "revision", 64, d.revision) || !validRevisionId(d.revision))
        return fail(Status::MalformedResponse, "malformed update response");
    d.id = id; d.requestId = rid;
    out = std::move(d);
    return r;
}

Result CommunityClient::remove(const std::string& id, const RevisionInfo& cur) {
    if (!isSafeIdentifier(id) || !validRevisionId(cur.revision)) return fail(Status::InvalidArgument, "invalid preset identifier");
    if (cfg_.accountId.empty() || cur.ownerId != cfg_.accountId) return fail(Status::NotOwner, "only the owner can delete this preset");
    JsonValue body = JsonValue::makeObject();
    body.set("expectedRevision", JsonValue::makeString(cur.revision));
    Call c; c.auth = true; c.retryable = false;
    c.req.method = HttpMethod::Delete; c.req.target = "/v1/presets/" + id; c.req.maxResponseBytes = kMaxJsonResponseBytes;
    c.req.body = core::writeJson(body);
    c.req.headers.emplace_back("Content-Type", "application/json");
    c.req.headers.emplace_back("If-Match", cur.revision);
    HttpResponse resp;
    Result r = execute(std::move(c), resp);
    if (r.status != Status::NoContent) return r.ok() ? fail(Status::UnexpectedStatus, "unexpected success status") : r;
    return r;  // cached copies remain local (tombstone semantics)
}

Result CommunityClient::report(const std::string& id, const std::string& rev, ReportReason reason, const std::string& text, std::string& reportId) {
    if (!isSafeIdentifier(id) || !validRevisionId(rev) || text.size() > 1000 || !noControl(text)) return fail(Status::InvalidArgument, "invalid report");
    JsonValue body = JsonValue::makeObject();
    body.set("presetId", JsonValue::makeString(id));
    body.set("revision", JsonValue::makeString(rev));
    body.set("reason", JsonValue::makeString(reasonText(reason)));
    if (!text.empty()) body.set("text", JsonValue::makeString(text));
    Call c; c.auth = true; c.retryable = false;
    c.req.method = HttpMethod::Post; c.req.target = "/v1/reports"; c.req.maxResponseBytes = kMaxJsonResponseBytes;
    c.req.body = core::writeJson(body);
    c.req.headers.emplace_back("Content-Type", "application/json");
    HttpResponse resp;
    Result r = execute(std::move(c), resp);
    if (r.status != Status::Created) return r.ok() ? fail(Status::UnexpectedStatus, "unexpected success status") : r;
    JsonValue v; std::string rid, rep;
    if (!parseEnvelope(resp.body, kMaxJsonResponseBytes, v, rid) || !getStr(v, "reportId", 64, rep) || rep.empty()) return fail(Status::MalformedResponse, "malformed report response");
    reportId = rep;
    return r;
}

}  // namespace cd::community
