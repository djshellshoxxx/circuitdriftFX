// CircuitDrift community: catalog client core over the injected Transport (CD-F04, 05 spec "Community
// browser and API", gate G06). Proposed HTTPS JSON API v1:
//   GET /v1/presets (q,cursor,limit<=50,tags,engineVersion; ETag/If-None-Match)
//   GET /v1/presets/{id}/revisions/{rev}  -> metadata, blobSha256, byteLength, download URL
//   POST /v1/presets (draft, Idempotency-Key)   POST /v1/presets/{id}/publish (explicit confirmation only)
//   PATCH /v1/presets/{id} (If-Match)   DELETE /v1/presets/{id}   POST /v1/reports
// Rules enforced here:
//  * Nothing touches the network until a method is called; guests may browse/download; everything
//    else requires a TokenProvider token. The token is sent only to API targets, never to blob URLs.
//  * Downloaded scene bytes are hashed (SHA-256 over the exact bytes) and compared with the revision's
//    blobSha256 BEFORE any parse; then core JSON limits + state::loadState + the publication
//    validator state::publishScene must accept them. Community content never bypasses those.
//  * Publish serialises only via state::publishScene; Publish is explicit (userConfirmed) and carries
//    an idempotency key that is reused by every retry so a retry cannot duplicate a publication.
//  * Revisions are immutable: the cache key is (id, revision). Ownership is checked client-side
//    (NotOwner, no request sent) and again by the server (403).
//  * Size caps on every response; typed Status for every outcome; messages never contain tokens.
// Threading: worker only. At most 2 concurrent requests (Busy beyond that).
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>
#include "community/OfflineCache.h"
#include "community/Transport.h"
#include "state/StateDocument.h"

namespace cd::community {

constexpr unsigned kMaxPageItems = 50;
constexpr std::size_t kMaxJsonResponseBytes = 1u << 20;

struct BrowseQuery {
    std::string q, cursor, engineVersion, ifNoneMatch;
    std::vector<std::string> tags;
    unsigned limit = kMaxPageItems;
};
struct PresetSummary {
    std::string id, title, author, latestRevision, engineModelVersion;
    std::vector<std::string> tags;
};
struct BrowsePage { std::vector<PresetSummary> items; std::string nextCursor, etag, requestId; };

struct RevisionInfo {
    std::string id, revision, ownerId, title, description, license, kind, engineModelVersion, blobSha256, downloadUrl, visibility, requestId;
    int minReaderVersion = 1;
    std::vector<std::string> requiredFeatures;
    std::uint64_t byteLength = 0;
};
struct LocalCapabilities {
    std::string engineModelVersion = std::string(state::kEngineModelVersion);
    int readerVersion = state::kReaderVersion;
    std::set<std::string> features;  // supported requiredFeatures
};
struct Compatibility { bool compatible = true; std::vector<std::string> reasons; };

struct ImportedScene {
    state::StateDocument doc;
    std::string bytes;       // exact validated bytes (cache/embedding source)
    std::string sha256;
    bool fromCache = false;
};
struct PublishRequest {
    std::string title, description, license, idempotencyKey;
    std::vector<std::string> tags;
    const state::StateDocument* scene = nullptr;
};
struct DraftInfo { std::string id, revision, requestId; };
enum class ReportReason : unsigned char { Spam, Offensive, Copyright, Malicious, Other };

struct ClientConfig {
    RetryPolicy retry;
    std::string accountId;            // for client-side ownership checks (not a credential)
    std::size_t maxBlobBytes = state::kSceneMaxBytes;
};
// Injectable environment (defaults: no sleeping, clock = 0, jitter = 0).
struct ClientEnv {
    std::function<void(int ms)> sleep;
    std::function<std::int64_t()> nowMs;
    std::function<double()> random01;
};

bool isSafeUrl(const std::string& url);  // https only, host present, no userinfo/control chars
std::string makeIdempotencyKey(const std::function<std::uint64_t()>& rng);  // 32 hex chars
bool isValidIdempotencyKey(const std::string& k);

class CommunityClient {
public:
    CommunityClient(Transport& transport, TokenProvider* tokens, OfflineCache* cache, ClientConfig cfg = {}, ClientEnv env = {});

    Result browse(const BrowseQuery& q, BrowsePage& out);
    Result getRevision(const std::string& id, const std::string& rev, RevisionInfo& out);
    // Cache first (preferCache) or network; always fully validated. Offline + miss => OfflineMiss.
    Result fetchScene(const std::string& id, const std::string& rev, bool preferCache, ImportedScene& out);
    Result createDraft(const PublishRequest& req, DraftInfo& out);
    Result publish(const std::string& id, const std::string& expectedRevision, bool userConfirmed, const std::string& idempotencyKey, DraftInfo& out);
    Result update(const std::string& id, const RevisionInfo& current, const PublishRequest& req, DraftInfo& out);
    Result remove(const std::string& id, const RevisionInfo& current);
    Result report(const std::string& id, const std::string& rev, ReportReason reason, const std::string& text, std::string& reportId);

    static Compatibility checkCompatibility(const RevisionInfo& info, const LocalCapabilities& caps);
    // Digest first, then JSON/state/publication validation. expectedSha/byteLength may be empty/0 to skip (cache path).
    static Result validateSceneBytes(const std::string& bytes, const std::string& expectedSha256, std::uint64_t expectedByteLength,
                                     std::size_t maxBytes, ImportedScene& out);

    std::uint64_t requestCount() const noexcept { return requests_.load(); }

private:
    struct Call {
        HttpRequest req;
        bool auth = false;
        bool retryable = false;   // reads and idempotency-keyed writes
    };
    Result execute(Call call, HttpResponse& response);
    Result fail(Status s, std::string msg) const;
    std::int64_t now() const { return env_.nowMs ? env_.nowMs() : 0; }
    Transport& transport_;
    TokenProvider* tokens_;
    OfflineCache* cache_;
    ClientConfig cfg_;
    ClientEnv env_;
    RateLimitGate gate_;
    std::atomic<int> inFlight_{0};
    std::atomic<std::uint64_t> requests_{0};
};

}  // namespace cd::community
