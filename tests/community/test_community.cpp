// cd_community tests (T04 client-side parts) against a mock Transport. Synthetic data only; G06
// (provider, auth domain, moderation operations) stays open. No sockets are used anywhere.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>
#if !defined(_WIN32)
#include <unistd.h>
#endif
#include "community/CommunityClient.h"
#include "community/OfflineCache.h"
#include "community/Sha256.h"
#include "community/Transport.h"
#include "core/Json.h"
#include "state/StateDocument.h"

namespace {
using namespace cd::community;
namespace fs = std::filesystem;
int failures = 0;
#define CHECK(c) do { if (!(c)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

const char* kSceneText = R"({"schemaVersion":1,"product":"CircuitDrift","kind":"scene","engineModelVersion":"0.0.1-dev",
 "minReaderVersion":1,"author":"fixture","title":"Synthetic scene","tags":[],"createdAt":"2026-10-30T12:00:00Z",
 "requiredFeatures":["CD-F20"],"blueprint":{"schemaVersion":1,"id":"example.rc","components":[{"id":"R1","modelId":"fixture.resistor"}]},
 "baseParameters":{"hpf.frequency":20.5,"quality.oversampling":3},"macros":{},"timeline":{},"faults":[],"seed":42})";

std::string canonicalScene() {
    cd::state::StateDocument d; std::string e, out;
    if (!cd::state::loadState(kSceneText, d, e)) { std::printf("fixture rejected: %s\n", e.c_str()); ++failures; return {}; }
    if (!cd::state::publishScene(d, out, e)) { std::printf("fixture unpublishable: %s\n", e.c_str()); ++failures; return {}; }
    return out;
}

std::string replace(std::string s, const std::string& a, const std::string& b) { auto p = s.find(a); if (p != std::string::npos) s.replace(p, a.size(), b); return s; }

struct Mock : Transport {
    std::function<HttpResponse(const HttpRequest&)> handler;
    std::vector<HttpRequest> log;
    HttpResponse send(const HttpRequest& r) override { log.push_back(r); return handler(r); }
};
struct Tokens : TokenProvider {
    std::string token = "SYNTHETIC-TOKEN-abc123XYZ";
    bool available = true; int refreshes = 0;
    bool accessToken(std::string& out, bool force) override { if (force) { ++refreshes; token += "-r"; } if (!available) return false; out = token; return true; }
};
HttpResponse reply(int status, std::string body = {}, Headers h = {}) { HttpResponse r; r.status = status; r.body = std::move(body); r.headers = std::move(h); return r; }
std::string errBody(const char* code, const std::string& msg, int retry = 0) {
    return std::string("{\"schemaVersion\":1,\"requestId\":\"req-9\",\"error\":{\"code\":\"") + code + "\",\"message\":\"" + msg + "\"" +
           (retry ? ",\"retryAfter\":" + std::to_string(retry) : "") + "}}";
}
const char* kListBody = R"({"schemaVersion":1,"requestId":"r1","items":[{"id":"p1","title":"One","author":"a","latestRevision":"3","engineModelVersion":"0.0.1-dev","tags":["x"]},
 {"id":"p2","title":"Two","latestRevision":"1"}],"nextCursor":"abc"})";

std::string revBody(const std::string& id, const std::string& rev, const std::string& bytes, const std::string& owner = "acct-1",
                    const std::string& url = "https://cdn.example.invalid/blob/1", const std::string& sha = {}, std::uint64_t len = 0) {
    return "{\"schemaVersion\":1,\"requestId\":\"r2\",\"revision\":{\"id\":\"" + id + "\",\"revision\":\"" + rev + "\",\"ownerId\":\"" + owner +
           "\",\"title\":\"T\",\"description\":\"d\",\"license\":\"CC0\",\"kind\":\"scene\",\"engineModelVersion\":\"0.0.1-dev\",\"minReaderVersion\":1,"
           "\"requiredFeatures\":[\"CD-F20\"],\"blobSha256\":\"" + (sha.empty() ? sha256Hex(bytes) : sha) + "\",\"byteLength\":" +
           std::to_string(len ? len : bytes.size()) + ",\"downloadUrl\":\"" + url + "\",\"visibility\":\"public\"}}";
}

struct Env {
    Mock mock; Tokens tokens; fs::path dir; OfflineCache cache; std::vector<int> sleeps; std::int64_t clock = 0;
    ClientConfig cfg; ClientEnv env;
    explicit Env(const fs::path& d, std::size_t cap = kDefaultCacheBytes) : dir(d), cache(CacheConfig{d, cap, 1310720}) {
        std::string e; fs::remove_all(d); cache.open(e);
        cfg.accountId = "acct-1";
        env.sleep = [this](int ms) { sleeps.push_back(ms); };
        env.nowMs = [this] { return clock; };
        env.random01 = [] { return 0.5; };
    }
    CommunityClient client() { return CommunityClient(mock, &tokens, &cache, cfg, env); }
};
fs::path tempDir(const char* name) {
#if defined(_WIN32)
    const long pid = 0;
#else
    const long pid = long(::getpid());
#endif
    return fs::temp_directory_path() / ("cd_community_test_" + std::to_string(pid) + "_" + name);
}

void testSha256() {
    CHECK(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(sha256Hex("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu") ==
          "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
    Sha256 h; const std::string a(1000, 'a');
    for (int i = 0; i < 1000; ++i) h.update(a.data(), a.size());
    const auto d = h.finish();
    CHECK(toHex(d.data(), d.size()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");  // one million 'a'
    // Padding boundaries and chunked updates agree with one-shot hashing.
    for (std::size_t n : {54u, 55u, 56u, 57u, 63u, 64u, 65u, 119u, 120u, 128u}) {
        const std::string s(n, 'x');
        Sha256 c; for (char ch : s) c.update(&ch, 1);
        const auto dd = c.finish();
        CHECK(toHex(dd.data(), dd.size()) == sha256Hex(s));
    }
    CHECK(sha256Hex(std::string(55, 'x')) != sha256Hex(std::string(56, 'x')));
    CHECK(isSha256Hex(sha256Hex("q")) && !isSha256Hex("ABC") && !isSha256Hex(std::string(64, 'G')));
}

void testStatusMapping() {
    struct Row { int http; Status s; };
    const Row rows[] = {{200, Status::Ok}, {201, Status::Created}, {204, Status::NoContent}, {304, Status::NotModified}, {400, Status::BadRequest},
        {401, Status::Unauthorized}, {403, Status::Forbidden}, {404, Status::NotFound}, {409, Status::Conflict}, {412, Status::PreconditionFailed},
        {413, Status::PayloadTooLarge}, {422, Status::Unprocessable}, {429, Status::RateLimited}, {500, Status::ServerError}, {502, Status::BadGateway},
        {503, Status::ServiceUnavailable}, {504, Status::GatewayTimeout}, {507, Status::ServerError}, {418, Status::UnexpectedStatus}, {302, Status::UnexpectedStatus}};
    for (auto& r : rows) CHECK(mapHttpResponse(reply(r.http)).status == r.s);
    HttpResponse t; t.timedOut = true; CHECK(mapHttpResponse(t).status == Status::Timeout);
    HttpResponse f; f.transportFailed = true; CHECK(mapHttpResponse(f).status == Status::TransportError);
    HttpResponse c; c.cancelled = true; CHECK(mapHttpResponse(c).status == Status::Cancelled);
    HttpResponse z; CHECK(mapHttpResponse(z).status == Status::TransportError);  // status 0
    CHECK(mapHttpResponse(reply(200)).ok() && mapHttpResponse(reply(304)).ok() && !mapHttpResponse(reply(404)).ok());
    // Error bodies and Retry-After.
    auto r = mapHttpResponse(reply(429, errBody("rate", "slow down", 7), {{"retry-after", "30"}}));
    CHECK(r.status == Status::RateLimited && r.retryAfterSeconds == 30 && r.code == "rate" && r.requestId == "req-9");
    r = mapHttpResponse(reply(429, errBody("rate", "slow down", 7)));
    CHECK(r.retryAfterSeconds == 7);
    r = mapHttpResponse(reply(503, "<html>not json</html>", {{"Retry-After", "abc"}}));
    CHECK(r.status == Status::ServiceUnavailable && r.retryAfterSeconds == 0 && r.message.empty());
    r = mapHttpResponse(reply(409, "{\"error\":[1,2,3]}"));
    CHECK(r.status == Status::Conflict);
    // Credential scrubbing.
    r = mapHttpResponse(reply(401, errBody("auth", "bad token SYNTHETIC-TOKEN-abc123XYZ and Bearer SYNTHETIC-TOKEN-abc123XYZ")), "SYNTHETIC-TOKEN-abc123XYZ");
    CHECK(r.message.find("SYNTHETIC") == std::string::npos && r.message.find("[redacted]") != std::string::npos);
    r = mapHttpResponse(reply(401, errBody("auth", "got Bearer zzz.yyy.xxx here")));
    CHECK(r.message.find("zzz") == std::string::npos);
    CHECK(sanitizeText("a\nb\tc\x01", {}, 100) == "a b c ");
    CHECK(sanitizeText(std::string(500, 'x'), {}, 256).size() == 256);
    CHECK(statusName(Status::DigestMismatch) == std::string("digest-mismatch"));
}

void testRetryPolicy() {
    RetryPolicy p;
    CHECK(p.delayMs(0, 0, 0.0) == 1000 && p.delayMs(1, 0, 0.0) == 2000 && p.delayMs(2, 0, 0.0) == 4000);
    CHECK(p.delayMs(0, 0, 1.0) == 1250 && p.delayMs(0, 0, 7.0) == 1250 && p.delayMs(0, 0, -3.0) == 1000);   // bounded jitter
    CHECK(p.delayMs(0, 5, 0.0) == 5000);                                                                       // Retry-After wins
    CHECK(p.shouldRetry(Status::ServiceUnavailable, 0, 0) && p.shouldRetry(Status::Timeout, 2, 0) && !p.shouldRetry(Status::Timeout, 3, 0));
    CHECK(!p.shouldRetry(Status::BadRequest, 0, 0) && !p.shouldRetry(Status::Conflict, 0, 0) && !p.shouldRetry(Status::RateLimited, 0, 120));
    RateLimitGate g; std::int64_t rem = 0;
    g.note(1000, 10); CHECK(g.blocked(5000, rem) && rem == 6000 && !g.blocked(11000, rem));
    g.note(0, 100000); CHECK(g.blocked(1000, rem) && rem <= 60000);  // capped
}

void testBrowse(const fs::path& base) {
    Env e(base / "browse"); auto c = e.client();
    CHECK(c.requestCount() == 0 && e.mock.log.empty());   // nothing happens until used
    e.mock.handler = [](const HttpRequest&) { return reply(200, kListBody, {{"ETag", "\"v1\""}}); };
    BrowsePage page; BrowseQuery q; q.q = "warm"; q.tags = {"a", "b"}; q.engineVersion = "0.0.1-dev"; q.cursor = "c0";
    auto r = c.browse(q, page);
    CHECK(r.status == Status::Ok && page.items.size() == 2 && page.items[0].id == "p1" && page.nextCursor == "abc" && page.etag == "\"v1\"" && page.requestId == "r1");
    CHECK(e.mock.log.size() == 1);
    const auto& req = e.mock.log[0];
    CHECK(req.method == HttpMethod::Get && req.target == "/v1/presets" && !findHeader(req.headers, "Authorization"));  // guest browse
    bool hasLimit = false; for (auto& kv : req.query) if (kv.first == "limit" && kv.second == "50") hasLimit = true;
    CHECK(hasLimit);
    // ETag revalidation.
    e.mock.handler = [](const HttpRequest& rq) { return findHeader(rq.headers, "If-None-Match") ? reply(304) : reply(200, kListBody); };
    q = BrowseQuery{}; q.ifNoneMatch = "\"v1\"";
    CHECK(c.browse(q, page).status == Status::NotModified);
    // Invalid limits never reach the network.
    const auto before = e.mock.log.size();
    q = BrowseQuery{}; q.limit = 51; CHECK(c.browse(q, page).status == Status::InvalidArgument);
    q.limit = 0; CHECK(c.browse(q, page).status == Status::InvalidArgument);
    CHECK(e.mock.log.size() == before);
    // Malformed / hostile bodies.
    struct Bad { const char* body; };
    const std::string tooMany = [] { std::string s = R"({"schemaVersion":1,"items":[)"; for (int i = 0; i < 51; ++i) s += std::string(i ? "," : "") + R"({"id":"p","title":"t","latestRevision":"1"})"; return s + "]}"; }();
    for (std::string body : {std::string("{not json"), std::string(""), std::string("[]"), std::string(R"({"schemaVersion":2,"items":[]})"), std::string(R"({"items":[]})"),
                             std::string(R"({"schemaVersion":1,"items":"x"})"), std::string(R"({"schemaVersion":1,"items":[{"id":"../x","title":"t","latestRevision":"1"}]})"),
                             std::string(R"({"schemaVersion":1,"items":[{"id":"a","title":5,"latestRevision":"1"}]})"), std::string(R"({"schemaVersion":1,"items":[{"id":"a","title":"t"}]})"),
                             std::string("{\"schemaVersion\":1,\"items\":[{\"id\":\"a\",\"title\":\"bad\\u0001\",\"latestRevision\":\"1\"}]}"),
                             std::string(R"({"schemaVersion":1,"items":[],"nextCursor":7})"), tooMany,
                             std::string(R"({"schemaVersion":1,"schemaVersion":1,"items":[]})")}) {
        e.mock.handler = [body](const HttpRequest&) { return reply(200, body); };
        const auto rr = c.browse(BrowseQuery{}, page);
        if (rr.status != Status::MalformedResponse) { ++failures; std::printf("FAIL malformed body accepted/mapped %s: %s\n", statusName(rr.status), body.substr(0, 40).c_str()); }
    }
    // Response size cap enforced even if the transport ignores maxResponseBytes.
    e.mock.handler = [](const HttpRequest&) { return reply(200, std::string((1u << 20) + 1, ' ')); };
    CHECK(c.browse(BrowseQuery{}, page).status == Status::ResponseTooLarge);
    // Every status code reaches the caller as its typed result.
    for (int code : {400, 401, 403, 404, 409, 412, 413, 422, 429, 500, 502, 503, 504}) {
        Env e2(base / "browse2");
        e2.mock.handler = [code](const HttpRequest&) { return reply(code, errBody("x", "msg")); };
        auto c2 = e2.client(); const auto rr = c2.browse(BrowseQuery{}, page);
        CHECK(rr.httpStatus == code && rr.code == "x" && !rr.ok());
        CHECK(rr.status == mapHttpResponse(reply(code)).status);
    }
    // Local limit: a 2xx that is not the expected 200.
    e.mock.handler = [](const HttpRequest&) { return reply(204); };
    CHECK(c.browse(BrowseQuery{}, page).status == Status::UnexpectedStatus);
}

void testRetries(const fs::path& base) {
    Env e(base / "retry"); auto c = e.client(); BrowsePage page;
    int n = 0;
    e.mock.handler = [&](const HttpRequest&) { return ++n <= 3 ? reply(503, errBody("down", "later")) : reply(200, kListBody); };
    auto r = c.browse(BrowseQuery{}, page);
    CHECK(r.status == Status::Ok && n == 4);
    CHECK(e.sleeps.size() == 3 && e.sleeps[0] == 1125 && e.sleeps[1] == 2250 && e.sleeps[2] == 4500);  // 1/2/4 s + 12.5 % jitter at u=.5
    // Exhausted: 1 + 3 retries, then the failure is returned.
    e.sleeps.clear(); n = 0; e.clock = 100000;
    e.mock.handler = [&](const HttpRequest&) { ++n; return reply(500, errBody("boom", "x")); };
    r = c.browse(BrowseQuery{}, page);
    CHECK(r.status == Status::ServerError && n == 4 && e.sleeps.size() == 3);
    // Non-retryable codes are not retried.
    n = 0; e.mock.handler = [&](const HttpRequest&) { ++n; return reply(404); };
    CHECK(c.browse(BrowseQuery{}, page).status == Status::NotFound && n == 1);
    // Retry-After is honoured (waits at least that long) and gates later requests locally.
    e.sleeps.clear(); n = 0; e.clock = 0;
    Env e3(base / "retry3"); auto c3 = e3.client();
    e3.mock.handler = [&](const HttpRequest&) { return ++n == 1 ? reply(429, errBody("rate", "slow"), {{"Retry-After", "5"}}) : reply(200, kListBody); };
    CHECK(c3.browse(BrowseQuery{}, page).status == Status::Ok && e3.sleeps.size() == 1 && e3.sleeps[0] >= 5000);
    // Retry-After beyond the wait cap: returned to the caller, no sleeping, and the gate blocks until it clears.
    Env e4(base / "retry4"); auto c4 = e4.client(); n = 0;
    e4.mock.handler = [&](const HttpRequest&) { ++n; return reply(429, errBody("rate", "slow"), {{"Retry-After", "600"}}); };
    r = c4.browse(BrowseQuery{}, page);
    CHECK(r.status == Status::RateLimited && r.retryAfterSeconds == 600 && n == 1 && e4.sleeps.empty());
    r = c4.browse(BrowseQuery{}, page);
    CHECK(r.status == Status::RateLimited && n == 1);                         // no request while gated
    e4.clock = 61000;
    r = c4.browse(BrowseQuery{}, page);
    CHECK(n == 2);                                                             // gate cleared (capped at 60 s)
    // Timeouts and transport failures are retried for reads.
    Env e5(base / "retry5"); auto c5 = e5.client(); n = 0;
    e5.mock.handler = [&](const HttpRequest&) { ++n; HttpResponse x; if (n == 1) x.timedOut = true; else if (n == 2) x.transportFailed = true; else return reply(200, kListBody); return x; };
    CHECK(c5.browse(BrowseQuery{}, page).status == Status::Ok && n == 3);
    // Cancellation maps to Cancelled and is not retried.
    Env e6(base / "retry6"); auto c6 = e6.client(); n = 0;
    e6.mock.handler = [&](const HttpRequest&) { ++n; HttpResponse x; x.cancelled = true; return x; };
    CHECK(c6.browse(BrowseQuery{}, page).status == Status::Cancelled && n == 1);
    // Concurrency cap: a transport that re-enters the client twice makes the third request Busy.
    Env e7(base / "retry7"); auto c7 = e7.client(); int depth = 0; Status inner = Status::Ok;
    e7.mock.handler = [&](const HttpRequest&) {
        const int my = ++depth;
        if (my < 3) { BrowsePage p2; const auto rr = c7.browse(BrowseQuery{}, p2); if (my == 2) inner = rr.status; }
        return reply(200, kListBody);
    };
    c7.browse(BrowseQuery{}, page);
    CHECK(inner == Status::Busy);
}

void testFetchAndValidation(const fs::path& base) {
    const std::string scene = canonicalScene();
    CHECK(!scene.empty());
    const std::string sha = sha256Hex(scene);
    Env e(base / "fetch"); auto c = e.client();
    auto serve = [&](const std::string& blob, const std::string& revJson) {
        e.mock.handler = [blob, revJson](const HttpRequest& rq) {
            if (rq.target.rfind("https://", 0) == 0) return reply(200, blob);
            return reply(200, revJson);
        };
    };
    serve(scene, revBody("p1", "3", scene));
    ImportedScene imp;
    auto r = c.fetchScene("p1", "3", true, imp);
    CHECK(r.status == Status::Ok && !imp.fromCache && imp.sha256 == sha && imp.doc.title == "Synthetic scene");
    CHECK(e.mock.log.size() == 2);
    CHECK(!findHeader(e.mock.log[1].headers, "Authorization"));            // blob host never sees a credential
    CHECK(e.cache.contains("p1", "3"));
    // Second fetch is served from the cache with zero network activity (offline).
    e.mock.log.clear();
    e.mock.handler = [](const HttpRequest&) { HttpResponse x; x.transportFailed = true; return x; };
    ImportedScene again; r = c.fetchScene("p1", "3", true, again);
    CHECK(r.status == Status::Ok && again.fromCache && again.bytes == scene && e.mock.log.empty());
    // Cache miss while offline: typed OfflineMiss.
    ImportedScene none; r = c.fetchScene("p1", "9", true, none);
    CHECK(r.status == Status::OfflineMiss);
    // Network-first request falls back to the cached copy when offline.
    ImportedScene fb; r = c.fetchScene("p1", "3", false, fb);
    CHECK(r.status == Status::Ok && fb.fromCache);

    // Digest mismatch: tampered (still valid) bytes are rejected before parse and never cached.
    Env e2(base / "fetch2"); auto c2 = e2.client();
    const std::string tampered = replace(scene, "Synthetic scene", "Synthetic scenf");
    e2.mock.handler = [&](const HttpRequest& rq) { return rq.target.rfind("https://", 0) == 0 ? reply(200, tampered) : reply(200, revBody("p1", "3", scene)); };
    r = c2.fetchScene("p1", "3", true, imp);
    CHECK(r.status == Status::DigestMismatch && !e2.cache.contains("p1", "3"));
    // Length mismatch (truncation).
    e2.mock.handler = [&](const HttpRequest& rq) { return rq.target.rfind("https://", 0) == 0 ? reply(200, scene.substr(0, scene.size() - 5)) : reply(200, revBody("p1", "3", scene)); };
    CHECK(c2.fetchScene("p1", "3", true, imp).status == Status::DigestMismatch);
    // Download larger than the declared byteLength is capped by the request and rejected.
    e2.mock.handler = [&](const HttpRequest& rq) { return rq.target.rfind("https://", 0) == 0 ? reply(200, scene + " ") : reply(200, revBody("p1", "3", scene)); };
    CHECK(c2.fetchScene("p1", "3", true, imp).status == Status::ResponseTooLarge);

    // Matching digest but hostile content: malformed JSON, schema violations, wrong kind, unpublishable content.
    struct Case { const char* name; std::string body; Status expect; };
    cd::state::StateDocument sd; std::string err;
    CHECK(cd::state::loadState(scene, sd, err));
    const std::vector<Case> cases = {
        {"malformed json", "{\"schemaVersion\":1,", Status::MalformedResponse},
        {"duplicate keys", replace(scene, "\"seed\":42", "\"seed\":42,\"seed\":43"), Status::MalformedResponse},
        {"not an object", "[1,2,3]", Status::ValidationFailed},
        {"wrong engine version", replace(scene, "0.0.1-dev", "9.9.9"), Status::ValidationFailed},
        {"unknown model", replace(scene, "fixture.resistor", "evil.model"), Status::ValidationFailed},
        {"session kind", replace(scene, "\"kind\":\"scene\"", "\"kind\":\"session\""), Status::ValidationFailed},
        {"future schema", replace(scene, "\"schemaVersion\":1,\"product\"", "\"schemaVersion\":99,\"product\""), Status::ValidationFailed},
        {"out of range value", replace(scene, "\"hpf.frequency\":20.5", "\"hpf.frequency\":1e30"), Status::ValidationFailed},
        {"non-finite", replace(scene, "\"hpf.frequency\":20.5", "\"hpf.frequency\":NaN"), Status::MalformedResponse},
        {"non-empty macros", replace(scene, "\"macros\":{}", "\"macros\":{\"m\":1}"), Status::ValidationFailed},
        {"embedded script", replace(scene, "\"faults\":[]", "\"faults\":[\"<script>\"]"), Status::ValidationFailed},
    };
    for (const auto& cs : cases) {
        Env e3(base / "fetch3"); auto c3 = e3.client();
        e3.mock.handler = [&](const HttpRequest& rq) { return rq.target.rfind("https://", 0) == 0 ? reply(200, cs.body) : reply(200, revBody("p1", "3", cs.body)); };
        ImportedScene x; r = c3.fetchScene("p1", "3", true, x);
        if (r.status != cs.expect) { ++failures; std::printf("FAIL case '%s': got %s expected %s (%s)\n", cs.name, statusName(r.status), statusName(cs.expect), r.message.c_str()); }
        if (e3.cache.contains("p1", "3")) { ++failures; std::printf("FAIL case '%s' was cached\n", cs.name); }
    }
    // Oversize document bound.
    ImportedScene big; const std::string huge(cd::state::kSceneMaxBytes + 1, ' ');
    CHECK(CommunityClient::validateSceneBytes(huge, {}, 0, cd::state::kSceneMaxBytes, big).status == Status::ResponseTooLarge);

    // Hostile revision metadata.
    const std::string good = revBody("p1", "3", scene);
    struct Meta { const char* name; std::string json; Status expect; };
    const std::vector<Meta> metas = {
        {"id swap", revBody("other", "3", scene), Status::MalformedResponse},
        {"rev swap", revBody("p1", "4", scene), Status::MalformedResponse},
        {"http url", revBody("p1", "3", scene, "acct-1", "http://cdn.example.invalid/x"), Status::MalformedResponse},
        {"userinfo url", revBody("p1", "3", scene, "acct-1", "https://user:pw@cdn.example.invalid/x"), Status::MalformedResponse},
        {"file url", revBody("p1", "3", scene, "acct-1", "file:///etc/passwd"), Status::MalformedResponse},
        {"bad digest", revBody("p1", "3", scene, "acct-1", "https://cdn.example.invalid/x", "NOTHEX"), Status::MalformedResponse},
        {"zero length", replace(good, "\"byteLength\":" + std::to_string(scene.size()), "\"byteLength\":0"), Status::MalformedResponse},
        {"huge length", replace(good, "\"byteLength\":" + std::to_string(scene.size()), "\"byteLength\":99999999999"), Status::MalformedResponse},
        {"missing revision", R"({"schemaVersion":1})", Status::MalformedResponse},
        {"not scene", replace(good, "\"kind\":\"scene\"", "\"kind\":\"session\""), Status::Incompatible},
        {"fractional length", replace(good, "\"byteLength\":" + std::to_string(scene.size()), "\"byteLength\":1.5"), Status::MalformedResponse},
    };
    for (const auto& m : metas) {
        Env e4(base / "fetch4"); auto c4 = e4.client(); RevisionInfo info;
        e4.mock.handler = [&](const HttpRequest&) { return reply(200, m.json); };
        r = c4.getRevision("p1", "3", info);
        if (r.status != m.expect) { ++failures; std::printf("FAIL meta '%s': got %s\n", m.name, statusName(r.status)); }
    }
    // Invalid identifiers never reach the network (path traversal).
    const auto n0 = e.mock.log.size();
    RevisionInfo info;
    for (const char* bad : {"../x", "a/b", "", "a b", "p1?x=1", "%2e%2e"}) CHECK(c.getRevision(bad, "1", info).status == Status::InvalidArgument);
    CHECK(c.getRevision("p1", "../1", info).status == Status::InvalidArgument && c.fetchScene("..", "1", true, imp).status == Status::InvalidArgument);
    CHECK(e.mock.log.size() == n0);

    // Compatibility.
    RevisionInfo ri;
    Env e5(base / "fetch5"); auto c5 = e5.client();
    e5.mock.handler = [&](const HttpRequest&) { return reply(200, good); };
    CHECK(c5.getRevision("p1", "3", ri).status == Status::Ok && ri.ownerId == "acct-1" && ri.byteLength == scene.size());
    LocalCapabilities caps; caps.features = {"CD-F20"};
    CHECK(CommunityClient::checkCompatibility(ri, caps).compatible);
    auto old = caps; old.features.clear(); auto cm = CommunityClient::checkCompatibility(ri, old);
    CHECK(!cm.compatible && cm.reasons.size() == 1);
    old = caps; old.engineModelVersion = "0.0.2"; CHECK(!CommunityClient::checkCompatibility(ri, old).compatible);
    old = caps; old.readerVersion = 0; RevisionInfo newer = ri; newer.minReaderVersion = 2; CHECK(!CommunityClient::checkCompatibility(newer, caps).compatible);
}

void testPublishAndOwnership(const fs::path& base) {
    const std::string sceneText = canonicalScene();
    cd::state::StateDocument doc; std::string err;
    CHECK(cd::state::loadState(sceneText, doc, err));
    Env e(base / "pub"); auto c = e.client();
    PublishRequest req; req.title = "My scene"; req.description = "desc"; req.license = "CC-BY-4.0"; req.tags = {"warm"}; req.scene = &doc;
    int n = 0, created = 0; std::map<std::string, int> seenKeys;
    e.mock.handler = [&](const HttpRequest& rq) {
        ++n;
        const std::string* k = findHeader(rq.headers, "Idempotency-Key");
        if (rq.method == HttpMethod::Post && rq.target == "/v1/presets") {
            if (n == 1) return reply(503, errBody("down", "retry me"));
            if (k && seenKeys[*k]++ == 0) ++created;  // server-side dedupe by key
            return reply(201, R"({"schemaVersion":1,"requestId":"q","id":"p77","revision":"1"})");
        }
        return reply(404);
    };
    DraftInfo d;
    // Duplicate-safe: the first attempt fails (503), the retry reuses the same key/body, exactly one creation.
    req.idempotencyKey = makeIdempotencyKey([] { return std::uint64_t(0x1234567890ABCDEFull); });
    CHECK(req.idempotencyKey.size() == 32 && isValidIdempotencyKey(req.idempotencyKey));
    auto r = c.createDraft(req, d);
    CHECK(r.status == Status::Created && d.id == "p77" && d.revision == "1" && created == 1 && n == 2);
    CHECK(e.mock.log.size() == 2 && e.mock.log[0].body == e.mock.log[1].body);
    CHECK(*findHeader(e.mock.log[0].headers, "Idempotency-Key") == *findHeader(e.mock.log[1].headers, "Idempotency-Key"));
    CHECK(e.mock.log[1].body.find(req.idempotencyKey) != std::string::npos);
    // Body carries the published (stripped) scene only; it parses and validates through loadState.
    cd::core::JsonValue body; CHECK(cd::core::parseJson(e.mock.log[1].body, body, err));
    CHECK(body.get("scene") && body.get("title") && body.get("license") && body.get("tags"));
    CHECK(cd::state::loadState(cd::core::writeJson(*body.get("scene")), doc, err));
    // A replayed key (server says 200) is accepted as the same draft.
    e.mock.handler = [&](const HttpRequest&) { return reply(200, R"({"schemaVersion":1,"id":"p77","revision":"1"})"); };
    CHECK(c.createDraft(req, d).ok());
    // Guards without any network use.
    e.mock.log.clear();
    auto bad = req; bad.idempotencyKey = "short"; CHECK(c.createDraft(bad, d).status == Status::InvalidArgument);
    bad = req; bad.title.clear(); CHECK(c.createDraft(bad, d).status == Status::InvalidArgument);
    bad = req; bad.license.clear(); CHECK(c.createDraft(bad, d).status == Status::InvalidArgument);
    bad = req; bad.scene = nullptr; CHECK(c.createDraft(bad, d).status == Status::InvalidArgument);
    bad = req; bad.tags.assign(17, "t"); CHECK(c.createDraft(bad, d).status == Status::InvalidArgument);
    cd::state::StateDocument session = doc; session.kind = cd::state::DocKind::Session;
    bad = req; bad.scene = &session; CHECK(c.createDraft(bad, d).status == Status::ValidationFailed);   // publishScene validator
    cd::state::StateDocument withMacros = doc; withMacros.macros = cd::core::JsonValue::makeObject(); withMacros.macros.set("m", cd::core::JsonValue::makeNumber(1));
    bad = req; bad.scene = &withMacros; CHECK(c.createDraft(bad, d).status == Status::ValidationFailed);
    CHECK(c.publish("p77", "1", false, req.idempotencyKey, d).status == Status::PublishNotConfirmed);  // explicit Publish only
    CHECK(c.publish("../x", "1", true, req.idempotencyKey, d).status == Status::InvalidArgument);
    CHECK(c.publish("p77", "1", true, "x", d).status == Status::InvalidArgument);
    CHECK(e.mock.log.empty());
    // Auth required for writes; guests never reach the network.
    Env g(base / "pub2"); g.tokens.available = false; auto cg = g.client();
    g.mock.handler = [](const HttpRequest&) { return reply(500); };
    CHECK(cg.createDraft(req, d).status == Status::AuthRequired && cg.publish("p77", "1", true, req.idempotencyKey, d).status == Status::AuthRequired);
    std::string rep; CHECK(cg.report("p77", "1", ReportReason::Spam, "", rep).status == Status::AuthRequired && g.mock.log.empty());
    CommunityClient noTokens(g.mock, nullptr, nullptr, g.cfg, g.env);
    CHECK(noTokens.createDraft(req, d).status == Status::AuthRequired && g.mock.log.empty());

    // Publish (confirmed), conflict, and server-side errors.
    e.mock.log.clear();
    e.mock.handler = [&](const HttpRequest& rq) {
        if (rq.target == "/v1/presets/p77/publish") {
            const auto* auth = findHeader(rq.headers, "Authorization");
            CHECK(auth && auth->rfind("Bearer ", 0) == 0);
            return reply(200, R"({"schemaVersion":1,"requestId":"z","revision":"2"})");
        }
        return reply(404);
    };
    r = c.publish("p77", "1", true, req.idempotencyKey, d);
    CHECK(r.status == Status::Ok && d.revision == "2" && e.mock.log.size() == 1 && e.mock.log[0].body.find("\"visibility\":\"public\"") != std::string::npos);
    e.mock.handler = [](const HttpRequest&) { return reply(409, errBody("conflict", "revision moved")); };
    CHECK(c.publish("p77", "1", true, req.idempotencyKey, d).status == Status::Conflict);
    e.mock.handler = [](const HttpRequest&) { return reply(200, "{\"oops\":1}"); };
    CHECK(c.publish("p77", "1", true, req.idempotencyKey, d).status == Status::MalformedResponse);

    // Ownership: client-side NotOwner (no request), server-side 403, owner success, 409/412.
    RevisionInfo mine; mine.id = "p77"; mine.revision = "2"; mine.ownerId = "acct-1";
    RevisionInfo theirs = mine; theirs.ownerId = "acct-2";
    e.mock.log.clear();
    CHECK(c.remove("p77", theirs).status == Status::NotOwner && c.update("p77", theirs, req, d).status == Status::NotOwner && e.mock.log.empty());
    Env noAcct(base / "pub3"); noAcct.cfg.accountId.clear(); auto cn = noAcct.client();
    CHECK(cn.remove("p77", mine).status == Status::NotOwner);
    e.mock.handler = [](const HttpRequest&) { return reply(403, errBody("forbidden", "not your preset")); };   // forged ownership claim
    CHECK(c.remove("p77", mine).status == Status::Forbidden && c.update("p77", mine, req, d).status == Status::Forbidden);
    e.mock.handler = [](const HttpRequest& rq) {
        CHECK(findHeader(rq.headers, "If-Match") && *findHeader(rq.headers, "If-Match") == "2");
        return rq.method == HttpMethod::Delete ? reply(204) : reply(200, R"({"schemaVersion":1,"revision":"3"})");
    };
    CHECK(c.remove("p77", mine).status == Status::NoContent);
    CHECK(c.update("p77", mine, req, d).status == Status::Ok && d.revision == "3");
    e.mock.handler = [](const HttpRequest&) { return reply(412, errBody("stale", "If-Match failed")); };
    CHECK(c.update("p77", mine, req, d).status == Status::PreconditionFailed && c.remove("p77", mine).status == Status::PreconditionFailed);
    e.mock.handler = [](const HttpRequest&) { return reply(200); };
    CHECK(c.remove("p77", mine).status == Status::UnexpectedStatus);                 // 200 where 204 is the contract
    // Removal of a published item: a later download of it is 404, cached copy stays usable offline.
    e.mock.handler = [](const HttpRequest&) { return reply(404, errBody("gone", "tombstone")); };
    ImportedScene imp; CHECK(c.fetchScene("p77", "2", false, imp).status == Status::NotFound);

    // Reports.
    e.mock.handler = [](const HttpRequest& rq) { CHECK(rq.target == "/v1/reports"); return reply(201, R"({"schemaVersion":1,"reportId":"rep-1"})"); };
    CHECK(c.report("p77", "2", ReportReason::Copyright, "text", rep).status == Status::Created && rep == "rep-1");
    CHECK(c.report("p77", "2", ReportReason::Other, std::string(1001, 'x'), rep).status == Status::InvalidArgument);

    // Token handling: 401 triggers exactly one refresh+retry; a second 401 is reported. Tokens never leak.
    Env t(base / "pub4"); auto ct = t.client(); int calls = 0;
    t.mock.handler = [&](const HttpRequest& rq) {
        ++calls;
        const auto* a = findHeader(rq.headers, "Authorization");
        if (!a || a->find("-r") == std::string::npos) return reply(401, errBody("expired", "token " + std::string(a ? *a : "") + " expired"));
        return reply(201, R"({"schemaVersion":1,"reportId":"ok"})");
    };
    CHECK(ct.report("p1", "1", ReportReason::Spam, "", rep).status == Status::Created && t.tokens.refreshes == 1 && calls == 2);
    t.mock.handler = [&](const HttpRequest& rq) { const auto* a = findHeader(rq.headers, "Authorization"); return reply(401, errBody("expired", "rejected " + std::string(a ? *a : "?"))); };
    const auto denied = ct.report("p1", "1", ReportReason::Spam, "", rep);
    CHECK(denied.status == Status::Unauthorized && t.tokens.refreshes == 2);
    CHECK(denied.message.find("SYNTHETIC-TOKEN") == std::string::npos && denied.code.find("SYNTHETIC") == std::string::npos && denied.requestId.find("SYNTHETIC") == std::string::npos);
    CHECK(denied.message.find("[redacted]") != std::string::npos);
    // Server echoing the token inside requestId/code is scrubbed too.
    t.mock.handler = [&](const HttpRequest&) { return reply(403, "{\"schemaVersion\":1,\"requestId\":\"" + t.tokens.token + "\",\"error\":{\"code\":\"" + t.tokens.token + "\",\"message\":\"" + t.tokens.token + "\"}}"); };
    const auto leak = ct.report("p1", "1", ReportReason::Spam, "", rep);
    CHECK(leak.status == Status::Forbidden && leak.message.find("SYNTHETIC") == std::string::npos && leak.code.find("SYNTHETIC") == std::string::npos && leak.requestId.find("SYNTHETIC") == std::string::npos);
    CHECK(isSafeUrl("https://a.example/x?y=1") && !isSafeUrl("http://a") && !isSafeUrl("https://") && !isSafeUrl("https://u@h/") && !isSafeUrl("https://a b/") && !isSafeUrl("javascript:alert(1)"));
}

void testCache(const fs::path& base) {
    const fs::path dir = base / "cache";
    fs::remove_all(dir);
    const std::string blobA(1000, 'A'), blobB(1000, 'B'), blobC(1000, 'C'), blobD(1000, 'D');
    std::string err;
    {
        OfflineCache c(CacheConfig{dir, 2500 + 3 * 70, 4096});  // room for 2 entries (each 1000 + 70-byte header)
        CHECK(!c.put("a", "1", "x", err));                       // not opened
        CHECK(c.open(err));
        std::string out;
        CHECK(c.get("a", "1", out) == CacheGet::Miss);
        CHECK(c.put("a", "1", blobA, err) && c.put("b", "1", blobB, err));
        CHECK(c.get("a", "1", out) == CacheGet::Hit && out == blobA);
        CHECK(c.entryCount() == 2 && c.totalBytes() == 2 * 1070);
        // LRU: a was just read, so adding c evicts b.
        CHECK(c.put("c", "1", blobC, err));
        CHECK(c.contains("a", "1") && !c.contains("b", "1") && c.contains("c", "1") && c.totalBytes() <= 2 * 1070 + 3 * 70);
        CHECK(c.get("b", "1", out) == CacheGet::Miss);
        // Pinned entries survive eviction pressure.
        CHECK(c.setPinned("a", "1", true));
        CHECK(c.put("d", "1", blobD, err));
        CHECK(c.contains("a", "1") && !c.contains("c", "1") && c.contains("d", "1"));
        CHECK(c.setPinned("d", "1", true));
        CHECK(!c.put("e", "1", std::string(1000, 'E'), err));    // nothing evictable: refused, cap holds
        CHECK(c.totalBytes() <= 2500 + 3 * 70);
        CHECK(c.setPinned("d", "1", false) && c.remove("d", "1") && !c.contains("d", "1"));
        // Bounds and key safety.
        CHECK(!c.put("a", "2", std::string(5000, 'z'), err) && err.find("bound") != std::string::npos);
        CHECK(!c.put("a", "2", "", err));
        for (const std::string& bad : {std::string("../etc"), std::string("a/b"), std::string("a\\b"), std::string(), std::string(".."), std::string("a b"), std::string("x\0y", 3)}) {
            CHECK(c.get(bad, "1", out) == CacheGet::InvalidKey);
            CHECK(!c.put(bad, "1", "data", err));
            CHECK(!c.put("a", bad, "data", err));
        }
        CHECK(c.get(std::string(65, 'a'), "1", out) == CacheGet::InvalidKey);
        // No temp files linger after successful writes; only <hex>.cdc (+ .pin markers) exist, all inside dir.
        for (auto& f : fs::directory_iterator(dir)) {
            const std::string n = f.path().filename().string();
            CHECK(n.size() >= 4 && (n.rfind(".cdc") == n.size() - 4 || n.rfind(".pin") == n.size() - 4));
        }
        CHECK(!fs::exists(base / "etc") && !fs::exists(base.parent_path() / "etc"));
    }
    // Persistence across reopen + integrity checks.
    OfflineCache c(CacheConfig{dir, 1u << 20, 4096});
    CHECK(c.open(err) && c.contains("a", "1"));
    std::string out;
    CHECK(c.get("a", "1", out) == CacheGet::Hit && out == blobA);
    const fs::path file = dir / OfflineCache::entryName("a", "1");
    {   // flip a payload byte: integrity failure, file removed
        std::fstream f(file, std::ios::in | std::ios::out | std::ios::binary); f.seekp(-10, std::ios::end); f.put('Z');
    }
    CHECK(c.get("a", "1", out) == CacheGet::Corrupt && !fs::exists(file) && !c.contains("a", "1"));
    CHECK(c.get("a", "1", out) == CacheGet::Miss);
    CHECK(c.put("t", "1", blobA, err));
    const fs::path tf = dir / OfflineCache::entryName("t", "1");
    fs::resize_file(tf, 20);                                            // truncated
    CHECK(c.get("t", "1", out) == CacheGet::Corrupt);
    CHECK(c.put("g", "1", blobA, err));
    { std::ofstream f(dir / OfflineCache::entryName("g", "1"), std::ios::binary | std::ios::trunc); f << "garbage"; }
    CHECK(c.get("g", "1", out) == CacheGet::Corrupt);
    CHECK(c.put("h", "1", blobA, err));
    { std::ofstream f(dir / OfflineCache::entryName("h", "1"), std::ios::binary | std::ios::trunc); f << std::string(10000, 'q'); }  // oversize
    CHECK(c.get("h", "1", out) == CacheGet::Corrupt);
    // Overwrite replaces atomically; stale temp files are cleaned at open.
    CHECK(c.put("o", "1", blobA, err) && c.put("o", "1", blobB, err) && c.get("o", "1", out) == CacheGet::Hit && out == blobB);
    { std::ofstream f(dir / "leftover.part"); f << "x"; }
    OfflineCache c2(CacheConfig{dir, 1u << 20, 4096}); CHECK(c2.open(err) && !fs::exists(dir / "leftover.part"));
    // Unrelated files in the directory are ignored, not interpreted.
    { std::ofstream f(dir / "notes.txt"); f << "x"; }
    OfflineCache c3(CacheConfig{dir, 1u << 20, 4096}); CHECK(c3.open(err) && c3.get("notes", "1", out) == CacheGet::Miss);
    CHECK(!OfflineCache(CacheConfig{}).open(err));
    // Write failure (destination is a regular file, not a directory) is reported, not thrown.
    { std::ofstream f(base / "afile"); f << "x"; }
    OfflineCache bad(CacheConfig{base / "afile", 1000, 100}); CHECK(!bad.open(err));
}

void testDigestAndHardCases() {
    // Empty / garbage inputs to the validator.
    ImportedScene x;
    CHECK(CommunityClient::validateSceneBytes("", sha256Hex(""), 0, 1000, x).status != Status::Ok);
    CHECK(CommunityClient::validateSceneBytes("{}", sha256Hex("{}"), 2, 1000, x).status == Status::ValidationFailed);
    CHECK(CommunityClient::validateSceneBytes("{}", sha256Hex("{ }"), 2, 1000, x).status == Status::DigestMismatch);
    const std::string deep(5000, '[');
    CHECK(CommunityClient::validateSceneBytes(deep, {}, 0, 100000, x).status == Status::MalformedResponse);  // depth bound
    const std::string utf = "{\"a\":\"\xff\xfe\"}";
    CHECK(CommunityClient::validateSceneBytes(utf, {}, 0, 100000, x).status == Status::MalformedResponse);   // invalid UTF-8
}
}  // namespace

int main() {
    const fs::path base = tempDir("all");
    fs::remove_all(base);
    fs::create_directories(base);
    testSha256();
    testStatusMapping();
    testRetryPolicy();
    testBrowse(base);
    testRetries(base);
    testFetchAndValidation(base);
    testPublishAndOwnership(base);
    testCache(base);
    testDigestAndHardCases();
    std::error_code ec; fs::remove_all(base, ec);
    if (failures) { std::printf("%d community check(s) FAILED\n", failures); return 1; }
    std::printf("community tests passed\n");
    return 0;
}
