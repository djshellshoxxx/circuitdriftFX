// CircuitDrift community: in-memory mock of the proposed catalogue service, for tests only (T04).
// Implements Transport directly (no sockets). SYNTHETIC: it models the documented API table, owner
// checks, ETag-less listing, idempotency-key replay and tombstones; it is not a real backend (G06 open).
// Fault injection: scripted per-call faults, forced bodies, blocking hook for concurrency tests.
// Threading: send() is mutex-guarded and may be called from several threads.
#pragma once
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include "community/Sha256.h"
#include "community/Transport.h"
#include "core/Json.h"

namespace cd::community {

class MockCatalogService final : public Transport {
public:
    struct Revision { std::string bytes, sha, title, license; };
    struct Preset {
        std::string owner;
        std::vector<Revision> revisions;
        bool published = false, deleted = false;
        std::vector<std::string> tags;
        int publishCount = 0;  // real publications (idempotent replays do not count)
    };
    enum class Fault : unsigned char { None, Status, Timeout, Network, ApplyThenTimeout };
    struct Scripted { Fault fault = Fault::None; int status = 0; std::string body; Headers headers; };

    static constexpr const char* kBlobHost = "https://cdn.mock.invalid/blobs/";

    // --- setup / inspection ---
    void addUser(const std::string& token, const std::string& user) { std::lock_guard<std::mutex> l(m_); users_[token] = user; }
    void expireToken(const std::string& token) { std::lock_guard<std::mutex> l(m_); users_.erase(token); }
    std::string seedPublished(const std::string& owner, const std::string& title, const std::string& sceneBytes,
                              const std::vector<std::string>& tags = {}) {
        std::lock_guard<std::mutex> l(m_);
        const std::string id = "p" + std::to_string(++nextId_);
        Preset& p = presets_[id];
        p.owner = owner; p.published = true; p.tags = tags; p.publishCount = 1;
        p.revisions.push_back({sceneBytes, sha256Hex(sceneBytes), title, "CC0-1.0"});
        blobs_[p.revisions.back().sha] = sceneBytes;
        return id;
    }
    void script(Scripted s) { std::lock_guard<std::mutex> l(m_); scripts_.push_back(std::move(s)); }
    void forceBodyFor(const std::string& targetContains, std::string body) {
        std::lock_guard<std::mutex> l(m_); forced_[targetContains] = std::move(body);
    }
    void tamperBlob(const std::string& sha, std::string bytes) { std::lock_guard<std::mutex> l(m_); blobs_[sha] = std::move(bytes); }
    void setSendHook(std::function<void()> h) { std::lock_guard<std::mutex> l(m_); hook_ = std::move(h); }
    int callCount() const { std::lock_guard<std::mutex> l(m_); return int(requests_.size()); }
    std::vector<TransportRequest> requests() const { std::lock_guard<std::mutex> l(m_); return requests_; }
    int publishCount(const std::string& id) const {
        std::lock_guard<std::mutex> l(m_);
        auto it = presets_.find(id);
        return it == presets_.end() ? -1 : it->second.publishCount;
    }
    int presetCount() const { std::lock_guard<std::mutex> l(m_); return int(presets_.size()); }
    int maxConcurrent() const { std::lock_guard<std::mutex> l(m_); return maxConcurrent_; }
    const Preset* preset(const std::string& id) const {
        std::lock_guard<std::mutex> l(m_);
        auto it = presets_.find(id);
        return it == presets_.end() ? nullptr : &it->second;
    }

    TransportResponse send(const TransportRequest& req, const CancelToken& cancel) override {
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> l(m_);
            requests_.push_back(req);
            ++active_;
            maxConcurrent_ = std::max(maxConcurrent_, active_);
            hook = hook_;
        }
        if (hook) hook();
        TransportResponse r = handle(req, cancel);
        std::lock_guard<std::mutex> l(m_);
        --active_;
        return r;
    }

private:
    using Json = core::JsonValue;

    static TransportResponse json(int status, Json body, Headers h = {}) {
        body.set("requestId", Json::makeString("req-mock"));
        body.set("schemaVersion", Json::makeNumber(1));
        TransportResponse r;
        r.status = status;
        r.body = core::writeJson(body);
        r.headers = std::move(h);
        return r;
    }
    static TransportResponse err(int status, const char* code, const char* msg, double retryAfter = -1) {
        Json b = Json::makeObject();
        b.set("code", Json::makeString(code));
        b.set("message", Json::makeString(msg));
        if (retryAfter >= 0) b.set("retryAfter", Json::makeNumber(retryAfter));
        return json(status, std::move(b));
    }
    static std::string hdr(const TransportRequest& r, const std::string& name) {
        for (const auto& [k, v] : r.headers) if (k == name) return v;
        return {};
    }
    static std::string qry(const TransportRequest& r, const std::string& name) {
        for (const auto& [k, v] : r.query) if (k == name) return v;
        return {};
    }

    TransportResponse handle(const TransportRequest& req, const CancelToken& cancel) {
        std::unique_lock<std::mutex> l(m_);
        if (cancel.cancelled()) { TransportResponse r; r.outcome = TransportOutcome::Cancelled; return r; }
        if (!scripts_.empty()) {
            Scripted s = scripts_.front();
            scripts_.pop_front();
            if (s.fault == Fault::Timeout) { TransportResponse r; r.outcome = TransportOutcome::Timeout; return r; }
            if (s.fault == Fault::Network) { TransportResponse r; r.outcome = TransportOutcome::NetworkError; return r; }
            if (s.fault == Fault::Status) {
                TransportResponse r;
                r.status = s.status; r.body = s.body; r.headers = s.headers;
                return r;
            }
            if (s.fault == Fault::ApplyThenTimeout) {
                (void)route(req);  // side effect lands on the server, the response is lost
                TransportResponse r; r.outcome = TransportOutcome::Timeout; return r;
            }
        }
        for (const auto& [needle, body] : forced_)
            if (req.target.find(needle) != std::string::npos) {
                TransportResponse r; r.status = 200; r.body = body; return r;
            }
        return route(req);
    }

    // Returns the user for a Bearer token, or empty (-> 401).
    std::string authUser(const TransportRequest& req) const {
        const std::string h = hdr(req, "Authorization");
        if (h.rfind("Bearer ", 0) != 0) return {};
        auto it = users_.find(h.substr(7));
        return it == users_.end() ? std::string() : it->second;
    }

    TransportResponse route(const TransportRequest& req) {
        const std::string& t = req.target;
        if (t.rfind(kBlobHost, 0) == 0) {
            auto it = blobs_.find(t.substr(std::string(kBlobHost).size()));
            if (it == blobs_.end()) return err(404, "not_found", "blob");
            TransportResponse r; r.status = 200; r.body = it->second; return r;
        }
        if (req.method == HttpMethod::Get && t == "/v1/presets") return list(req);
        if (req.method == HttpMethod::Post && t == "/v1/presets") return create(req);
        if (req.method == HttpMethod::Post && t == "/v1/reports") return report(req);
        const std::string prefix = "/v1/presets/";
        if (t.rfind(prefix, 0) != 0) return err(404, "not_found", "route");
        std::string rest = t.substr(prefix.size());
        const auto slash = rest.find('/');
        const std::string id = rest.substr(0, slash);
        const std::string tail = slash == std::string::npos ? "" : rest.substr(slash);
        auto it = presets_.find(id);
        if (req.method == HttpMethod::Get && tail.rfind("/revisions/", 0) == 0) return revision(req, it, id, tail.substr(11));
        if (req.method == HttpMethod::Post && tail == "/publish") return publish(req, it);
        if (req.method == HttpMethod::Patch && tail.empty()) return patch(req, it);
        if (req.method == HttpMethod::Delete && tail.empty()) return remove(req, it);
        return err(404, "not_found", "route");
    }

    using It = std::map<std::string, Preset>::iterator;

    TransportResponse list(const TransportRequest& req) {
        std::size_t start = 0;
        if (!qry(req, "cursor").empty()) start = std::strtoull(qry(req, "cursor").c_str(), nullptr, 10);
        const std::size_t limit = std::strtoull(qry(req, "limit").c_str(), nullptr, 10);
        const std::string needle = qry(req, "q");
        Json items = Json::makeArray();
        std::size_t idx = 0, emitted = 0;
        bool more = false;
        for (const auto& [id, p] : presets_) {
            if (!p.published || p.deleted) continue;
            if (!needle.empty() && p.revisions.back().title.find(needle) == std::string::npos) continue;
            if (idx++ < start) continue;
            if (emitted == limit) { more = true; break; }
            Json o = Json::makeObject();
            o.set("id", Json::makeString(id));
            o.set("title", Json::makeString(p.revisions.back().title));
            o.set("author", Json::makeString(p.owner));
            Json tags = Json::makeArray();
            for (const auto& tg : p.tags) tags.array.push_back(Json::makeString(tg));
            o.set("tags", std::move(tags));
            o.set("latestRevision", Json::makeNumber(double(p.revisions.size())));
            items.array.push_back(std::move(o));
            ++emitted;
        }
        Json b = Json::makeObject();
        b.set("items", std::move(items));
        b.set("nextCursor", more ? Json::makeString(std::to_string(start + emitted)) : Json());
        return json(200, std::move(b), {{"ETag", "\"mock-etag\""}});
    }

    TransportResponse revision(const TransportRequest&, It it, const std::string& id, const std::string& revStr) {
        const std::size_t rev = std::strtoull(revStr.c_str(), nullptr, 10);
        if (it == presets_.end() || it->second.deleted || rev < 1 || rev > it->second.revisions.size())
            return err(404, "not_found", "revision");
        const Revision& r = it->second.revisions[rev - 1];
        Json b = Json::makeObject();
        b.set("presetId", Json::makeString(id));
        b.set("revision", Json::makeNumber(double(rev)));
        b.set("title", Json::makeString(r.title));
        b.set("blobSha256", Json::makeString(r.sha));
        b.set("byteLength", Json::makeNumber(double(r.bytes.size())));
        b.set("downloadUrl", Json::makeString(std::string(kBlobHost) + r.sha));
        b.set("license", Json::makeString(r.license));
        return json(200, std::move(b));
    }

    // Replays the stored response for a repeated idempotency key; otherwise runs `fn` and stores it.
    template <class Fn>
    TransportResponse idempotent(const TransportRequest& req, Fn&& fn) {
        const std::string key = hdr(req, "Idempotency-Key");
        if (key.empty()) return fn();
        auto it = replay_.find(key);
        if (it != replay_.end()) return it->second;
        TransportResponse r = fn();
        if (r.status >= 200 && r.status < 300) replay_[key] = r;
        return r;
    }

    TransportResponse create(const TransportRequest& req) {
        const std::string user = authUser(req);
        if (user.empty()) return err(401, "unauthorized", "token expired or missing");
        return idempotent(req, [&]() -> TransportResponse {
            Json body; std::string why;
            if (!core::parseJson(req.body, body, why) || !body.isObject() || !body.get("scene") || !body.get("title") ||
                !body.get("idempotencyKey") || body.get("idempotencyKey")->string != hdr(req, "Idempotency-Key"))
                return err(400, "invalid", "bad body");
            const std::string bytes = core::writeJson(*body.get("scene"));
            if (bytes.size() > 1310720) return err(413, "too_large", "scene");
            const std::string id = "p" + std::to_string(++nextId_);
            Preset& p = presets_[id];
            p.owner = user;
            const Json* lic = body.get("license");
            p.revisions.push_back({bytes, sha256Hex(bytes), body.get("title")->string, lic ? lic->string : ""});
            blobs_[p.revisions.back().sha] = bytes;
            Json b = Json::makeObject();
            b.set("id", Json::makeString(id));
            b.set("revision", Json::makeNumber(1));
            return json(201, std::move(b));
        });
    }

    TransportResponse publish(const TransportRequest& req, It it) {
        const std::string user = authUser(req);
        if (user.empty()) return err(401, "unauthorized", "token expired or missing");
        if (it == presets_.end() || it->second.deleted) return err(404, "not_found", "preset");
        if (it->second.owner != user) return err(403, "forbidden", "not the owner");
        return idempotent(req, [&]() -> TransportResponse {
            Json body; std::string why;
            if (!core::parseJson(req.body, body, why) || !body.get("expectedRevision")) return err(400, "invalid", "bad body");
            if (std::size_t(body.get("expectedRevision")->number) != it->second.revisions.size())
                return err(409, "revision_conflict", "stale revision");
            it->second.published = true;
            ++it->second.publishCount;
            Json b = Json::makeObject();
            b.set("id", Json::makeString(it->first));
            b.set("revision", Json::makeNumber(double(it->second.revisions.size())));
            return json(200, std::move(b));
        });
    }

    TransportResponse patch(const TransportRequest& req, It it) {
        const std::string user = authUser(req);
        if (user.empty()) return err(401, "unauthorized", "token expired or missing");
        if (it == presets_.end() || it->second.deleted) return err(404, "not_found", "preset");
        if (it->second.owner != user) return err(403, "forbidden", "not the owner");
        return idempotent(req, [&]() -> TransportResponse {
            if (std::strtoull(hdr(req, "If-Match").c_str(), nullptr, 10) != it->second.revisions.size())
                return err(409, "revision_conflict", "If-Match stale");
            Json body; std::string why;
            if (!core::parseJson(req.body, body, why) || !body.get("title")) return err(400, "invalid", "bad body");
            Revision r = it->second.revisions.back();
            r.title = body.get("title")->string;
            if (const Json* sc = body.get("scene")) { r.bytes = core::writeJson(*sc); r.sha = sha256Hex(r.bytes); }
            blobs_[r.sha] = r.bytes;
            it->second.revisions.push_back(std::move(r));
            it->second.published = false;  // new private revision
            Json b = Json::makeObject();
            b.set("id", Json::makeString(it->first));
            b.set("revision", Json::makeNumber(double(it->second.revisions.size())));
            return json(200, std::move(b));
        });
    }

    TransportResponse remove(const TransportRequest& req, It it) {
        const std::string user = authUser(req);
        if (user.empty()) return err(401, "unauthorized", "token expired or missing");
        if (it == presets_.end() || it->second.deleted) return err(404, "not_found", "preset");
        if (it->second.owner != user) return err(403, "forbidden", "not the owner");
        return idempotent(req, [&]() -> TransportResponse {
            if (std::strtoull(qry(req, "expectedRevision").c_str(), nullptr, 10) != it->second.revisions.size())
                return err(409, "revision_conflict", "stale revision");
            it->second.deleted = true;  // tombstone; blob objects stay for already-saved projects
            Json b = Json::makeObject();
            b.set("id", Json::makeString(it->first));
            b.set("revision", Json::makeNumber(double(it->second.revisions.size())));
            b.set("tombstone", Json::makeBool(true));
            return json(200, std::move(b));
        });
    }

    TransportResponse report(const TransportRequest& req) {
        if (authUser(req).empty()) return err(401, "unauthorized", "token expired or missing");
        return idempotent(req, [&]() -> TransportResponse {
            Json b = Json::makeObject();
            b.set("reportId", Json::makeString("r" + std::to_string(++nextReport_)));
            return json(201, std::move(b));
        });
    }

    mutable std::mutex m_;
    std::map<std::string, std::string> users_, blobs_;
    std::map<std::string, Preset> presets_;
    std::map<std::string, TransportResponse> replay_;
    std::map<std::string, std::string> forced_;
    std::deque<Scripted> scripts_;
    std::vector<TransportRequest> requests_;
    std::function<void()> hook_;
    int nextId_ = 0, nextReport_ = 0, active_ = 0, maxConcurrent_ = 0;
};

}  // namespace cd::community
