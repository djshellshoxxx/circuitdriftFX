// T04 community client contract tests against MockCatalogService (SYNTHETIC mock; no network,
// no backend: gate G06 stays open). Also SHA-256 NIST vectors and LocalCache LRU/pinning.
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include "community/CatalogClient.h"
#include "community/LocalCache.h"
#include "community/MockCatalogService.h"
#include "community/Sha256.h"
#include "core/Json.h"
#include "state/StateDocument.h"

namespace {
using namespace cd;
using namespace cd::community;
int failures = 0;
#define CHECK(c) do { if (!(c)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

const char* kScene = R"({"schemaVersion":1,"product":"CircuitDrift","kind":"scene","engineModelVersion":"0.0.1-dev",
 "minReaderVersion":1,"author":"fixture","title":"Synthetic scene","tags":[],"createdAt":"2026-10-30T12:00:00Z",
 "requiredFeatures":["CD-F20"],"blueprint":{"schemaVersion":1,"id":"example.rc","components":[{"id":"R1","modelId":"fixture.resistor"}]},
 "baseParameters":{"hpf.frequency":20.5,"quality.oversampling":3},"macros":{},"timeline":{},"faults":[],"seed":42,
 "ui":{"filePath":"SYNTHETIC_PRIVATE_PATH"},"x-token":"SYNTHETIC_TOKEN"})";

const std::string kSecret = "SYNTHETIC-SECRET-TOKEN-0123456789";

struct FakeClock final : Clock {
    std::int64_t t = 0;
    std::vector<std::int64_t> sleeps;
    std::function<void()> onSleep;
    std::int64_t nowMs() override { return t; }
    bool sleepMs(std::int64_t ms, const CancelToken& c) override {
        sleeps.push_back(ms);
        if (onSleep) onSleep();
        if (c.cancelled()) return false;
        t += ms;
        return true;
    }
};

state::StateDocument sceneDoc() {
    state::StateDocument d; std::string e;
    CHECK(state::loadState(kScene, d, e));
    return d;
}
std::string sceneBytes() {
    std::string out, e;
    CHECK(state::publishScene(sceneDoc(), out, e));
    return out;
}

struct Fixture {
    MockCatalogService svc;
    FakeClock clock;
    SeededRng rng{7};
    LocalCache cache;
    IdempotencyJournal journal;
    std::vector<std::string> logs;
    CatalogClient client;
    Fixture() : client(make()) {
        svc.addUser(kSecret, "alice");
        svc.addUser("bob-token", "bob");
    }
    ClientConfig make() {
        ClientConfig c;
        c.transport = &svc; c.clock = &clock; c.rng = &rng; c.cache = &cache; c.journal = &journal;
        c.log = [this](const std::string& s) { logs.push_back(s); };
        return c;
    }
    void login() { client.setAuthToken(AuthToken(kSecret)); }
};

DraftMetadata meta() { return {"My preset", "desc", "CC0-1.0", {"synthetic"}}; }

// ---- SHA-256 / cache -----------------------------------------------------------------------
void sha256Vectors() {
    CHECK(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(sha256Hex("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu") ==
          "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
    Sha256 h;
    const std::string chunk(1000, 'a');
    for (int i = 0; i < 1000; ++i) h.update(chunk);
    CHECK(toHex(h.finish()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    // Padding boundaries 55/56/63/64/65 bytes: one-shot equals byte-wise streaming.
    for (std::size_t n : {0u, 1u, 54u, 55u, 56u, 57u, 63u, 64u, 65u, 119u, 120u, 128u}) {
        const std::string s(n, 'x');
        Sha256 g;
        for (char c : s) g.update(&c, 1);
        CHECK(toHex(g.finish()) == sha256Hex(s));
    }
    CHECK(isSha256Hex(sha256Hex("x")) && !isSha256Hex("ABC") && !isSha256Hex(std::string(64, 'G')));
}

void cacheLru() {
    LocalCache c(100, [](const std::string&) { return std::size_t(40); });  // injected accounting
    const std::string a = "a", b = "b", d = "d", e = "e";
    CHECK(c.put(sha256Hex(a), a) && c.put(sha256Hex(b), b));
    CHECK(c.totalBytes() == 80);
    CHECK(c.get(sha256Hex(a)).has_value());            // a is now most recent
    CHECK(c.put(sha256Hex(d), d));                     // evicts b (LRU)
    CHECK(c.contains(sha256Hex(a)) && !c.contains(sha256Hex(b)) && c.contains(sha256Hex(d)));
    CHECK(!c.put(sha256Hex(e), "not-e"));              // key must equal digest of bytes
    CHECK(c.setPinned(sha256Hex(a), true) && c.setPinned(sha256Hex(d), true));
    CHECK(c.put(sha256Hex(e), e));                     // everything else pinned: newcomer is the only evictable
    CHECK(c.contains(sha256Hex(a)) && c.contains(sha256Hex(d)));
    LocalCache big(250u * 1024u * 1024u);
    CHECK(big.capacity() == 262144000u);
    const std::string huge(1000, 'z');
    LocalCache tiny(10);
    CHECK(!tiny.put(sha256Hex(huge), huge));           // larger than capacity, unpinned: rejected
}

// ---- T04 -----------------------------------------------------------------------------------
void zeroNetworkUntilUsed() {
    Fixture f;
    f.login();
    f.client.setOffline(false);
    CHECK(f.svc.callCount() == 0);
    CHECK(f.clock.sleeps.empty());
    CancelToken cx;
    f.client.setOffline(true);
    CHECK(f.client.listPresets({}, cx).error.kind == ErrorKind::Offline);
    CHECK(f.svc.callCount() == 0);
    // Guest write attempts and unconfirmed publish also never reach the transport.
    f.client.setOffline(false);
    f.client.clearAuthToken();
    CHECK(f.client.createDraft(meta(), sceneDoc(), "op", cx).error.kind == ErrorKind::Unauthorized);
    f.login();
    CHECK(f.client.publish("p1", 1, false, "op", cx).error.kind == ErrorKind::ConfirmRequired);
    CHECK(f.svc.callCount() == 0);
}

void browseAndDownload() {
    Fixture f;
    const std::string bytes = sceneBytes();
    const std::string id = f.svc.seedPublished("carol", "Warm Tape", bytes, {"tape"});
    CancelToken cx;
    auto page = f.client.listPresets({"", "", "", {}, 50}, cx);
    CHECK(page.ok() && page.value.items.size() == 1 && page.value.items[0].id == id && !page.value.fromCache);
    CHECK(page.value.etag == "\"mock-etag\"" && page.value.nextCursor.empty());
    auto rev = f.client.getRevision(id, 1, cx);
    CHECK(rev.ok() && rev.value.byteLength == std::int64_t(bytes.size()));
    CHECK(rev.value.blobSha256 == sha256Hex(bytes));
    auto dl = f.client.fetchScene(rev.value, cx, false);
    CHECK(dl.ok() && !dl.value.fromCache && dl.value.doc.kind == state::DocKind::Scene);
    CHECK(f.cache.contains(rev.value.blobSha256));
    // Transport got the required timeouts and a size cap; guest download carries no Authorization.
    const auto reqs = f.svc.requests();
    CHECK(reqs.back().connectTimeoutMs == 10000 && reqs.back().totalTimeoutMs == 30000);
    CHECK(reqs.back().maxResponseBytes == bytes.size());
    f.login();
    const int before = f.svc.callCount();
    f.cache.erase(rev.value.blobSha256);
    CHECK(f.client.fetchScene(rev.value, cx).ok());
    const auto all = f.svc.requests();
    for (std::size_t i = std::size_t(before); i < all.size(); ++i)
        for (const auto& [k, v] : all[i].headers) CHECK(k != "Authorization");  // never to the CDN
    // Cache hit: no transport call.
    const int n = f.svc.callCount();
    auto again = f.client.fetchScene(rev.value, cx);
    CHECK(again.ok() && again.value.fromCache && f.svc.callCount() == n);
    // Pagination: limit bounds and cursor round trip.
    CHECK(f.client.listPresets({"", "", "", {}, 51}, cx).error.kind == ErrorKind::InvalidRequest);
    CHECK(f.client.listPresets({"", "", "", {}, 0}, cx).error.kind == ErrorKind::InvalidRequest);
    f.svc.seedPublished("carol", "Second", bytes);
    auto p1 = f.client.listPresets({"", "", "", {}, 1}, cx);
    CHECK(p1.ok() && p1.value.items.size() == 1 && !p1.value.nextCursor.empty());
    auto p2 = f.client.listPresets({"", p1.value.nextCursor, "", {}, 1}, cx);
    CHECK(p2.ok() && p2.value.items.size() == 1 && p2.value.items[0].id != p1.value.items[0].id);
    // Path-injection ids are rejected locally.
    const int m = f.svc.callCount();
    CHECK(f.client.getRevision("../admin", 1, cx).error.kind == ErrorKind::InvalidRequest);
    CHECK(f.client.getRevision("a/b", 1, cx).error.kind == ErrorKind::InvalidRequest && f.svc.callCount() == m);
}

void offlineBrowseUsesCache() {
    Fixture f;
    const std::string bytes = sceneBytes();
    const std::string id = f.svc.seedPublished("carol", "Warm Tape", bytes);
    CancelToken cx;
    ListQuery q{"", "", "", {}, 50};
    auto live = f.client.listPresets(q, cx);
    auto rev = f.client.getRevision(id, 1, cx);
    CHECK(live.ok() && rev.ok() && f.client.fetchScene(rev.value, cx).ok());
    const int n = f.svc.callCount();
    f.client.setOffline(true);
    auto off = f.client.listPresets(q, cx);
    CHECK(off.ok() && off.value.fromCache && off.value.items.size() == 1 && f.svc.callCount() == n);
    CHECK(f.client.getRevision(id, 1, cx).ok() && f.client.fetchScene(rev.value, cx).value.fromCache);
    CHECK(f.svc.callCount() == n);
    ListQuery other{"never-seen", "", "", {}, 50};
    CHECK(f.client.listPresets(other, cx).error.kind == ErrorKind::Offline);
    // Network failure (not just explicit offline) falls back to the cached page after retries.
    f.client.setOffline(false);
    for (int i = 0; i < 4; ++i) f.svc.script({MockCatalogService::Fault::Network, 0, "", {}});
    auto fb = f.client.listPresets(q, cx);
    CHECK(fb.ok() && fb.value.fromCache && fb.attempts == 4);
}

void timeoutAndRetryPolicy() {
    Fixture f;
    CancelToken cx;
    for (int i = 0; i < 4; ++i) f.svc.script({MockCatalogService::Fault::Timeout, 0, "", {}});
    auto r = f.client.listPresets({}, cx);
    CHECK(r.error.kind == ErrorKind::Timeout && r.attempts == 4 && f.svc.callCount() == 4);
    CHECK(f.clock.sleeps.size() == 3);
    const std::int64_t base[3] = {1000, 2000, 4000};
    for (int i = 0; i < 3 && i < int(f.clock.sleeps.size()); ++i)
        CHECK(f.clock.sleeps[i] >= base[i] && f.clock.sleeps[i] <= base[i] + 250);  // bounded jitter
    // A transient failure then success is retried transparently.
    Fixture g;
    g.svc.script({MockCatalogService::Fault::Status, 503, "", {}});
    auto ok = g.client.listPresets({}, cx);
    CHECK(ok.ok() && ok.attempts == 2 && g.clock.sleeps.size() == 1);
}

void retryAfterHonoured() {
    CancelToken cx;
    Fixture f;
    f.svc.script({MockCatalogService::Fault::Status, 429, "", {{"Retry-After", "7"}}});
    auto r = f.client.listPresets({}, cx);
    CHECK(r.ok() && r.attempts == 2 && f.clock.sleeps.size() == 1 && f.clock.sleeps[0] >= 7000);
    Fixture g;  // retryAfter in the JSON error body
    g.svc.script({MockCatalogService::Fault::Status, 429,
                  R"({"requestId":"r","schemaVersion":1,"code":"rate_limited","message":"slow","retryAfter":12})", {}});
    auto r2 = g.client.listPresets({}, cx);
    CHECK(r2.ok() && g.clock.sleeps.size() == 1 && g.clock.sleeps[0] >= 12000);
    Fixture h;  // an hour-long hint is surfaced, never slept on
    h.svc.script({MockCatalogService::Fault::Status, 429,
                  R"({"requestId":"r","schemaVersion":1,"code":"rate_limited","message":"slow","retryAfter":3600})", {}});
    auto r3 = h.client.listPresets({}, cx);
    CHECK(r3.error.kind == ErrorKind::RateLimited && r3.error.retryAfterMs == 3600000 && r3.attempts == 1 && h.clock.sleeps.empty());
    CHECK(r3.error.code == "rate_limited");
}

void cancellation() {
    Fixture f;
    CancelToken pre;
    pre.cancel();
    CHECK(f.client.listPresets({}, pre).error.kind == ErrorKind::Cancelled && f.svc.callCount() == 0);
    // Cancel while waiting for a retry delay.
    CancelToken cx;
    f.svc.script({MockCatalogService::Fault::Timeout, 0, "", {}});
    f.clock.onSleep = [&] { cx.cancel(); };
    auto r = f.client.listPresets({}, cx);
    CHECK(r.error.kind == ErrorKind::Cancelled && f.svc.callCount() == 1);
    // Cancel during an in-flight transport call.
    Fixture g;
    CancelToken c2;
    g.svc.setSendHook([&] { c2.cancel(); });
    CHECK(g.client.listPresets({}, c2).error.kind == ErrorKind::Cancelled);
}

void expiredAuthAndOwnership() {
    Fixture f;
    CancelToken cx;
    const std::string bytes = sceneBytes();
    f.login();
    auto d = f.client.createDraft(meta(), sceneDoc(), "op1", cx);
    CHECK(d.ok() && d.value.revision == 1);
    f.svc.expireToken(kSecret);
    const int n = f.svc.callCount();
    auto pub = f.client.publish(d.value.id, 1, true, "op2", cx);
    CHECK(pub.error.kind == ErrorKind::Unauthorized && pub.attempts == 1 && f.svc.callCount() == n + 1);
    CHECK(f.clock.sleeps.empty());  // no retry on 401
    // Forged ownership: bob publishes alice's draft.
    f.svc.addUser(kSecret, "alice");
    f.client.setAuthToken(AuthToken("bob-token"));
    const int m = f.svc.callCount();
    auto forged = f.client.publish(d.value.id, 1, true, "op3", cx);
    CHECK(forged.error.kind == ErrorKind::Forbidden && forged.attempts == 1 && f.svc.callCount() == m + 1);
    CHECK(f.svc.publishCount(d.value.id) == 0);
    CHECK(f.client.remove(d.value.id, 1, "op4", cx).error.kind == ErrorKind::Forbidden);
    CHECK(f.client.patch(d.value.id, 1, meta(), nullptr, "op5", cx).error.kind == ErrorKind::Forbidden);
    CHECK(f.svc.preset(d.value.id) && !f.svc.preset(d.value.id)->deleted);
    (void)bytes;
}

void conflictAndStatusMapping() {
    Fixture f;
    CancelToken cx;
    f.login();
    auto d = f.client.createDraft(meta(), sceneDoc(), "op1", cx);
    const int n = f.svc.callCount();
    auto stale = f.client.publish(d.value.id, 5, true, "op2", cx);  // wrong expectedRevision -> 409
    CHECK(stale.error.kind == ErrorKind::Conflict && stale.error.httpStatus == 409 && stale.attempts == 1);
    CHECK(f.svc.callCount() == n + 1 && f.clock.sleeps.empty());
    CHECK(f.client.patch(d.value.id, 9, meta(), nullptr, "op3", cx).error.kind == ErrorKind::Conflict);
    CHECK(f.client.getRevision("nope", 1, cx).error.kind == ErrorKind::NotFound);
    const struct { int s; ErrorKind k; } map[] = {{400, ErrorKind::InvalidRequest}, {401, ErrorKind::Unauthorized},
        {403, ErrorKind::Forbidden}, {404, ErrorKind::NotFound}, {409, ErrorKind::Conflict}, {413, ErrorKind::TooLarge}};
    for (const auto& m : map) {
        Fixture g;
        g.svc.script({MockCatalogService::Fault::Status, m.s, "", {}});
        auto r = g.client.listPresets({}, cx);
        CHECK(r.error.kind == m.k && r.attempts == 1 && g.clock.sleeps.empty());  // none of these retry
    }
    Fixture g;
    for (int i = 0; i < 4; ++i) g.svc.script({MockCatalogService::Fault::Status, 503, "", {}});
    CHECK(g.client.listPresets({}, cx).error.kind == ErrorKind::Unavailable && g.svc.callCount() == 4);
}

void maliciousJson() {
    CancelToken cx;
    const std::string pre = R"({"requestId":"r","schemaVersion":1,)";
    struct Case { const char* name; std::string body; };
    std::string deep = pre + R"("items":)";
    for (int i = 0; i < 60; ++i) deep += "[";
    for (int i = 0; i < 60; ++i) deep += "]";
    deep += "}";
    const Case cases[] = {
        {"duplicate keys", R"({"requestId":"r","schemaVersion":1,"items":[],"items":[]})"},
        {"nan", pre + R"("items":[],"x":NaN})"},
        {"overflow number", pre + R"("items":[],"x":1e999})"},
        {"deep", deep},
        {"not object", "[1,2,3]"},
        {"missing requestId", R"({"schemaVersion":1,"items":[]})"},
        {"missing schemaVersion", R"({"requestId":"r","items":[]})"},
        {"future schemaVersion", R"({"requestId":"r","schemaVersion":2,"items":[]})"},
        {"fractional schemaVersion", R"({"requestId":"r","schemaVersion":1.5,"items":[]})"},
        {"huge", pre + R"("pad":")" + std::string(2 * 1024 * 1024, 'a') + "\"}"},
        {"trailing data", pre + R"("items":[]} garbage)"},
        {"item bad id", pre + R"("items":[{"id":"../x","title":"t","author":"a","tags":[],"latestRevision":1}]})"},
        {"item control chars", pre + R"("items":[{"id":"p1","title":"a\u0000b","author":"a","tags":[],"latestRevision":1}]})"},
        {"too many items", pre + R"("items":[)" + [] {
             std::string s;
             for (int i = 0; i < 51; ++i) s += std::string(i ? "," : "") + R"({"id":"p)" + std::to_string(i) + R"(","title":"t","author":"a","tags":[],"latestRevision":1})";
             return s; }() + "]}"},
    };
    for (const auto& c : cases) {
        Fixture f;
        f.svc.forceBodyFor("/v1/presets", c.body);
        auto r = f.client.listPresets({"", "", "", {}, 50}, cx);
        const bool rejected = r.error.kind == ErrorKind::MalformedResponse || r.error.kind == ErrorKind::TooLarge;
        if (!rejected) std::printf("  not rejected: %s (%s)\n", c.name, errorKindName(r.error.kind));
        CHECK(rejected);
        CHECK(r.value.items.empty());
    }
    // Malicious revision metadata: hostile URL schemes, creds in URL, oversize byteLength, bad digest.
    const char* badKey[] = {"downloadUrl", "downloadUrl", "downloadUrl", "byteLength", "blobSha256"};
    const char* badVal[] = {"\"http://evil.invalid/x\"", "\"https://u:p@evil.invalid/x\"", "\"javascript:alert(1)\"",
                            "99999999", "\"ZZ\""};
    for (int i = 0; i < 5; ++i) {
        Fixture f;
        std::string body = pre + R"x("presetId":"p1","revision":1,"title":"t","blobSha256":")x" + std::string(64, 'a') +
                           R"x(","byteLength":10,"downloadUrl":"https://cdn.example.invalid/b","license":"x"})x";
        const std::string needle = std::string("\"") + badKey[i] + "\":";
        const auto at = body.find(needle) + needle.size();
        const auto end = body.find_first_of(",}", at);
        body.replace(at, end - at, badVal[i]);
        f.svc.forceBodyFor("/revisions/", body);
        CHECK(f.client.getRevision("p1", 1, cx).error.kind == ErrorKind::MalformedResponse);
    }
}

void digestMismatchBeforeParse() {
    Fixture f;
    CancelToken cx;
    const std::string bytes = sceneBytes();
    const std::string id = f.svc.seedPublished("carol", "Warm Tape", bytes);
    auto rev = f.client.getRevision(id, 1, cx);
    // Garbage with the wrong digest -> DigestMismatch (not a parse error: parse never ran).
    std::string garbage(bytes.size(), '{');
    f.svc.tamperBlob(rev.value.blobSha256, garbage);
    auto r = f.client.fetchScene(rev.value, cx);
    CHECK(r.error.kind == ErrorKind::DigestMismatch && !f.cache.contains(rev.value.blobSha256));
    // One flipped byte in an otherwise valid scene is also a digest error, not a clean load.
    std::string flipped = bytes;
    flipped[flipped.find("fixture")] = 'F';
    f.svc.tamperBlob(rev.value.blobSha256, flipped);
    CHECK(f.client.fetchScene(rev.value, cx).error.kind == ErrorKind::DigestMismatch);
    // Wrong length (even with a matching prefix) is rejected.
    f.svc.tamperBlob(rev.value.blobSha256, bytes + " ");
    CHECK(f.client.fetchScene(rev.value, cx).error.kind != ErrorKind::None);
    // Correct digest but not a valid scene: rejected by the StateDocument parser, never cached.
    const std::string junk = R"({"schemaVersion":1,"kind":"scene","title":"x"})";
    const std::string id2 = f.svc.seedPublished("carol", "Junk", junk);
    auto rev2 = f.client.getRevision(id2, 1, cx);
    auto r2 = f.client.fetchScene(rev2.value, cx);
    CHECK(r2.error.kind == ErrorKind::InvalidScene && !f.cache.contains(rev2.value.blobSha256));
    // Session-kind documents are not acceptable community scenes.
    state::StateDocument sd = sceneDoc();
    sd.kind = state::DocKind::Session;
    std::string sessionText;
    try { sessionText = state::saveState(sd); } catch (...) {}
    if (!sessionText.empty()) {
        const std::string id3 = f.svc.seedPublished("carol", "Sess", sessionText);
        auto rev3 = f.client.getRevision(id3, 1, cx);
        CHECK(!f.client.fetchScene(rev3.value, cx).ok());
    }
    // Oversize download aborted by the cap.
    RevisionInfo big = rev.value;
    big.byteLength = 10;
    f.svc.tamperBlob(big.blobSha256, bytes);
    CHECK(f.client.fetchScene(big, cx).error.kind == ErrorKind::TooLarge);
}

void duplicateRetriesDoNotDuplicatePublication() {
    Fixture f;
    CancelToken cx;
    f.login();
    // createDraft: response lost after the server applied it; the retry replays by key.
    f.svc.script({MockCatalogService::Fault::ApplyThenTimeout, 0, "", {}});
    auto d = f.client.createDraft(meta(), sceneDoc(), "create-1", cx);
    CHECK(d.ok() && d.attempts == 2 && f.svc.presetCount() == 1);
    // publish: applied but lost; retry with the same key leaves exactly one publication.
    f.svc.script({MockCatalogService::Fault::ApplyThenTimeout, 0, "", {}});
    auto p = f.client.publish(d.value.id, 1, true, "pub-1", cx);
    CHECK(p.ok() && p.attempts == 2 && f.svc.publishCount(d.value.id) == 1);
    const auto reqs = f.svc.requests();
    std::string k1, k2;
    for (const auto& r : reqs)
        if (r.target.find("/publish") != std::string::npos)
            for (const auto& [k, v] : r.headers) if (k == "Idempotency-Key") (k1.empty() ? k1 : k2) = v;
    CHECK(!k1.empty() && k1 == k2);
    // Retries exhausted (4 lost responses), then the user re-submits the same action: same key, still one publication.
    Fixture g;
    g.login();
    auto d2 = g.client.createDraft(meta(), sceneDoc(), "c", cx);
    for (int i = 0; i < 4; ++i) g.svc.script({MockCatalogService::Fault::ApplyThenTimeout, 0, "", {}});
    auto lost = g.client.publish(d2.value.id, 1, true, "pub-x", cx);
    CHECK(lost.error.kind == ErrorKind::Timeout && lost.attempts == 4);
    CHECK(g.journal.snapshot().count("pub-x") == 1);  // key persisted across the failed action
    auto again = g.client.publish(d2.value.id, 1, true, "pub-x", cx);
    CHECK(again.ok() && g.svc.publishCount(d2.value.id) == 1);
    CHECK(g.journal.snapshot().count("pub-x") == 0);  // forgotten after a definitive result
    // Writes also survive 429 with a key (same key reused), and distinct actions get distinct keys.
    Fixture h;
    h.login();
    h.svc.script({MockCatalogService::Fault::Status, 429, "", {{"Retry-After", "2"}}});
    auto c1 = h.client.createDraft(meta(), sceneDoc(), "A", cx);
    auto c2 = h.client.createDraft(meta(), sceneDoc(), "B", cx);
    CHECK(c1.ok() && c2.ok() && c1.value.id != c2.value.id && h.svc.presetCount() == 2);
    auto rr = h.client.report(c1.value.id, 1, ReportReason::Spam, "text", "rep", cx);
    CHECK(rr.ok() && !rr.value.empty());
}

void removalKeepsLocalCopy() {
    Fixture f;
    CancelToken cx;
    f.login();
    auto d = f.client.createDraft(meta(), sceneDoc(), "c", cx);
    CHECK(f.client.publish(d.value.id, 1, true, "p", cx).ok());
    auto rev = f.client.getRevision(d.value.id, 1, cx);
    auto dl = f.client.fetchScene(rev.value, cx, true);
    CHECK(dl.ok());
    auto del = f.client.remove(d.value.id, 1, "d", cx);
    CHECK(del.ok() && f.svc.preset(d.value.id)->deleted);
    CHECK(f.client.getRevision(d.value.id, 1, cx).error.kind == ErrorKind::NotFound);  // gone remotely
    const int n = f.svc.callCount();
    auto local = f.client.fetchScene(rev.value, cx);  // cached copy remains usable, no network
    CHECK(local.ok() && local.value.fromCache && f.svc.callCount() == n);
    // Deleting twice / stale revision does not alter the cache.
    CHECK(f.client.remove(d.value.id, 1, "d2", cx).error.kind == ErrorKind::NotFound);
    CHECK(f.cache.contains(rev.value.blobSha256));
    // Patch creates a new private revision.
    Fixture g;
    g.login();
    auto d2 = g.client.createDraft(meta(), sceneDoc(), "c", cx);
    auto pt = g.client.patch(d2.value.id, 1, meta(), nullptr, "pt", cx);
    CHECK(pt.ok() && pt.value.revision == 2);
}

void concurrencyCap() {
    Fixture f;
    const std::string id = f.svc.seedPublished("carol", "Warm Tape", sceneBytes());
    f.svc.setSendHook([] { std::this_thread::sleep_for(std::chrono::milliseconds(25)); });
    std::vector<std::thread> ts;
    std::atomic<int> okCount{0};
    CancelToken cx;
    for (int i = 0; i < 8; ++i)
        ts.emplace_back([&] { if (f.client.listPresets({}, cx).ok()) ++okCount; });
    for (auto& t : ts) t.join();
    CHECK(okCount == 8);
    CHECK(f.client.maxObservedConcurrency() <= kMaxConcurrentRequests && f.client.maxObservedConcurrency() >= 1);
    CHECK(f.svc.maxConcurrent() <= 2);
    (void)id;
}

void tokenNeverLeaks() {
    Fixture f;
    CancelToken cx;
    f.login();
    AuthToken tok(kSecret);
    CHECK(tok.toString().find(kSecret) == std::string::npos);
    CHECK(tok.toString().find("REDACTED") != std::string::npos);
    const std::string id = f.svc.seedPublished("carol", "Warm Tape", sceneBytes());
    auto d = f.client.createDraft(meta(), sceneDoc(), "c", cx);
    f.client.publish(d.value.id, 1, true, "p", cx);
    f.client.patch(d.value.id, 2, meta(), nullptr, "x", cx);  // 409 path
    f.client.report(id, 1, ReportReason::Other, "hmm", "r", cx);
    f.client.remove(d.value.id, 1, "d", cx);
    f.svc.expireToken(kSecret);
    f.client.publish(d.value.id, 1, true, "p2", cx);  // 401 path
    int authHeaders = 0;
    for (const auto& r : f.svc.requests()) {
        CHECK(r.body.find(kSecret) == std::string::npos);
        CHECK(r.target.find(kSecret) == std::string::npos);
        for (const auto& [k, v] : r.query) CHECK(k.find(kSecret) == std::string::npos && v.find(kSecret) == std::string::npos);
        for (const auto& [k, v] : r.headers) {
            if (k == "Authorization") { ++authHeaders; CHECK(v == "Bearer " + kSecret); }
            else CHECK(v.find(kSecret) == std::string::npos);
        }
    }
    CHECK(authHeaders > 0);
    for (const auto& line : f.logs) CHECK(line.find(kSecret) == std::string::npos);
    CHECK(!f.logs.empty());
    for (const auto& [k, v] : f.journal.snapshot()) CHECK(k.find(kSecret) == std::string::npos && v.find(kSecret) == std::string::npos);
    // Presets serialized locally never carry it either.
    CHECK(state::saveState(sceneDoc()).find(kSecret) == std::string::npos);
}

}  // namespace

int main() {
    sha256Vectors();
    cacheLru();
    zeroNetworkUntilUsed();
    browseAndDownload();
    offlineBrowseUsesCache();
    timeoutAndRetryPolicy();
    retryAfterHonoured();
    cancellation();
    expiredAuthAndOwnership();
    conflictAndStatusMapping();
    maliciousJson();
    digestMismatchBeforeParse();
    duplicateRetriesDoNotDuplicatePublication();
    removalKeepsLocalCopy();
    concurrencyCap();
    tokenNeverLeaks();
    std::printf("community T04: %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
