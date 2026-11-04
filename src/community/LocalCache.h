// CircuitDrift community: in-memory local blob cache keyed by SHA-256, 250 MiB LRU over
// unpinned entries; pinned entries are never evicted. Size accounting is injectable.
// The key must equal the SHA-256 of the stored bytes (checked on put and on get), so a corrupt
// or mislabelled entry can never be served as a different blob.
// Owner module: cd_community. Threading: worker threads; internally mutex-guarded. Never audio.
#pragma once
#include <cstddef>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace cd::community {

constexpr std::size_t kCacheCapacityBytes = 250u * 1024u * 1024u;

class LocalCache {
public:
    using SizeFn = std::function<std::size_t(const std::string& bytes)>;
    explicit LocalCache(std::size_t capacityBytes = kCacheCapacityBytes, SizeFn sizeOf = {});

    // Rejects (false) a digest/bytes mismatch or an unpinned entry larger than the capacity.
    // Re-putting an existing key refreshes recency and may upgrade (never downgrade) pinning.
    bool put(const std::string& sha256, std::string bytes, bool pinned = false);
    std::optional<std::string> get(const std::string& sha256);  // refreshes LRU; verifies digest
    bool contains(const std::string& sha256) const;
    bool setPinned(const std::string& sha256, bool pinned);
    bool erase(const std::string& sha256);  // explicit user removal only; pinned also removed
    std::size_t totalBytes() const;
    std::size_t entryCount() const;
    std::size_t capacity() const noexcept { return capacity_; }

private:
    struct Entry { std::string bytes; std::size_t size; bool pinned; std::list<std::string>::iterator lru; };
    void evictLocked();
    std::size_t capacity_;
    SizeFn sizeOf_;
    mutable std::mutex m_;
    std::unordered_map<std::string, Entry> map_;
    std::list<std::string> lru_;  // front = most recent
    std::size_t total_ = 0;
};

}  // namespace cd::community
