// CircuitDrift community: client contract for the proposed HTTPS JSON API v1 (05-state-presets-community).
// Gate G06 OPEN: talks only to an abstract Transport; no network, no credentials here.
//
// Guarantees: construction performs no I/O (zero transport calls until a method is called); at most 2
// requests in flight; transport gets 10 s connect / 30 s total timeouts; reads retry 3x at 1/2/4 s plus
// bounded jitter honouring Retry-After; writes retry only when they carry an idempotency key (persisted
// per operation in IdempotencyJournal so a retry reuses the same key); 401/403/404/409/413/400 never
// retry; responses are bounded JSON (duplicate keys / non-finite / depth rejected) with requestId and
// schemaVersion; blob digest (SHA-256 over exact bytes) is validated BEFORE the scene is parsed, then
// the scene goes through cd::state::loadState. Publish needs an explicit confirm flag.
// Tokens go only into the Authorization header; they are never in bodies, query strings or log lines.
// Owner module: cd_community. Threading: worker threads (methods block); never the audio thread.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <list>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <string>
#include <vector>
#include "community/LocalCache.h"
#include "community/Transport.h"
#include "state/StateDocument.h"

namespace cd::community {

enum class ErrorKind : unsigned char {
    None, InvalidRequest /*400*/, Unauthorized /*401*/, Forbidden /*403*/, NotFound /*404*/,
    Conflict /*409*/, TooLarge /*413 or size cap*/, RateLimited /*429*/, Unavailable /*503/5xx*/,
    Timeout, Cancelled, Network, MalformedResponse, DigestMismatch, InvalidScene,
    ConfirmRequired, Offline
};
const char* errorKindName(ErrorKind k) noexcept;

struct ApiError {
    ErrorKind kind = ErrorKind::None;
    int httpStatus = 0;
    std::string code, message, requestId;
    std::int64_t retryAfterMs = -1;  // -1 = none given
    bool ok() const noexcept { return kind == ErrorKind::None; }
};

constexpr std::size_t kMetadataResponseMaxBytes = 1048576;  // list/revision/ack responses
constexpr std::size_t kSceneBlobMaxBytes = state::kSceneMaxBytes;  // 1.25 MiB
constexpr int kMaxConcurrentRequests = 2;
constexpr int kConnectTimeoutMs = 10000;
constexpr int kTotalTimeoutMs = 30000;
constexpr int kMaxReadRetries = 3;
constexpr int kMaxPageSize = 50;
constexpr std::int64_t kMaxRetryAfterMs = 30000;  // longer server hints are surfaced, not slept on

struct PresetSummary {
    std::string id, title, author;
    std::vector<std::string> tags;
    std::int64_t latestRevision = 0;
};
struct ListQuery {
    std::string q, cursor, engineVersion;
    std::vector<std::string> tags;
    int limit = kMaxPageSize;  // 1..50
};
struct ListPage {
    std::vector<PresetSummary> items;
    std::string nextCursor, etag, requestId;
    bool fromCache = false;
};
struct RevisionInfo {
    std::string presetId, title, blobSha256, downloadUrl, license;
    std::int64_t revision = 0, byteLength = 0;
};
struct PresetRef { std::string id; std::int64_t revision = 0; };
enum class ReportReason : unsigned char { Copyright, Malicious, Abuse, Spam, Other };
struct DraftMetadata { std::string title, description, license; std::vector<std::string> tags; };

template <class T>
struct Result {
    ApiError error;
    T value{};
    int attempts = 0;
    bool ok() const noexcept { return error.ok(); }
};
struct SceneDownload { state::StateDocument doc; std::string bytes; std::string sha256; bool fromCache = false; };

// Persisted per operation: the first use of an opId mints a key; every retry/re-submit of the same
// opId reuses it, so the server can dedupe. A host may persist snapshot() across restarts.
class IdempotencyJournal {
public:
    std::string keyFor(const std::string& opId, Rng& rng);
    void forget(const std::string& opId);
    std::map<std::string, std::string> snapshot() const;
    void restore(std::map<std::string, std::string> s);

private:
    mutable std::mutex m_;
    std::map<std::string, std::string> keys_;
};

// Small bounded LRU map (caller synchronises). Least recently used entry is evicted at capacity.
template <class V>
class LruMap {
public:
    explicit LruMap(std::size_t cap = 64) : cap_(cap ? cap : 1) {}
    const V* get(const std::string& k) {
        auto it = map_.find(k);
        if (it == map_.end()) return nullptr;
        order_.splice(order_.begin(), order_, it->second.second);
        return &it->second.first;
    }
    void put(const std::string& k, V v) {
        auto it = map_.find(k);
        if (it != map_.end()) { it->second.first = std::move(v); order_.splice(order_.begin(), order_, it->second.second); return; }
        order_.push_front(k);
        map_.emplace(k, std::make_pair(std::move(v), order_.begin()));
        while (map_.size() > cap_) { map_.erase(order_.back()); order_.pop_back(); }
    }
    std::size_t size() const noexcept { return map_.size(); }

private:
    std::size_t cap_;
    std::list<std::string> order_;
    std::map<std::string, std::pair<V, std::list<std::string>::iterator>> map_;
};

struct ClientConfig {
    Transport* transport = nullptr;  // required
    Clock* clock = nullptr;          // required
    Rng* rng = nullptr;              // required (jitter + idempotency keys)
    LocalCache* cache = nullptr;     // optional; scene blobs keyed by SHA-256
    IdempotencyJournal* journal = nullptr;  // optional; client owns a private one if null
    std::function<void(const std::string&)> log;  // optional; lines never contain credentials
    std::int64_t maxJitterMs = 250;
};

class CatalogClient {
public:
    explicit CatalogClient(ClientConfig cfg);
    CatalogClient(const CatalogClient&) = delete;
    CatalogClient& operator=(const CatalogClient&) = delete;

    void setAuthToken(AuthToken t);          // login result; held in memory only
    void clearAuthToken();
    void setOffline(bool offline) noexcept;  // offline: cache only, zero transport calls

    // Reads (guest allowed). listPresets falls back to the last cached page when the network fails.
    Result<ListPage> listPresets(const ListQuery& q, const CancelToken& cancel);
    Result<RevisionInfo> getRevision(const std::string& id, std::int64_t rev, const CancelToken& cancel);
    // Cache-first for immutable blobs; otherwise downloads, checks size cap and digest, then parses.
    Result<SceneDownload> fetchScene(const RevisionInfo& info, const CancelToken& cancel, bool pin = false);

    // Writes (login required). opId identifies the user action for idempotency.
    Result<PresetRef> createDraft(const DraftMetadata& meta, const state::StateDocument& scene,
                                  const std::string& opId, const CancelToken& cancel);
    // `confirmed` must be true (explicit final click); false => ConfirmRequired, no transport call.
    Result<PresetRef> publish(const std::string& id, std::int64_t expectedRevision, bool confirmed,
                              const std::string& opId, const CancelToken& cancel);
    Result<PresetRef> patch(const std::string& id, std::int64_t ifMatchRevision, const DraftMetadata& meta,
                            const state::StateDocument* newScene, const std::string& opId,
                            const CancelToken& cancel);
    // Tombstones the remote item only; locally cached copies are left untouched.
    Result<PresetRef> remove(const std::string& id, std::int64_t expectedRevision, const std::string& opId,
                             const CancelToken& cancel);
    Result<std::string> report(const std::string& id, std::int64_t rev, ReportReason reason,
                               const std::string& text, const std::string& opId, const CancelToken& cancel);

    int maxObservedConcurrency() const;
    std::size_t cachedMetadataEntries() const;

private:
    struct Call;
    struct Outcome { ApiError error; TransportResponse response; int attempts = 0; };
    Outcome run(const Call& c, const CancelToken& cancel);
    bool acquire(const CancelToken& cancel);
    void release();
    void logLine(const std::string& s) const;
    std::string idempotencyKey(const std::string& opId);

    ClientConfig cfg_;
    IdempotencyJournal ownJournal_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    int inFlight_ = 0, maxInFlight_ = 0;
    AuthToken token_;
    bool offline_ = false;
    static constexpr std::size_t kMetadataCacheEntries = 64;
    LruMap<ListPage> listCache_{kMetadataCacheEntries};
    LruMap<RevisionInfo> revisionCache_{kMetadataCacheEntries};
    std::mutex rngMutex_;  // cfg_.rng is not thread-safe: every use goes through this lock
};

}  // namespace cd::community
