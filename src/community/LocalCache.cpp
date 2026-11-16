#include "community/LocalCache.h"
#include "community/Sha256.h"
#include <utility>

namespace cd::community {

LocalCache::LocalCache(std::size_t capacityBytes, SizeFn sizeOf)
    : capacity_(capacityBytes), sizeOf_(std::move(sizeOf)) {
    if (!sizeOf_) sizeOf_ = [](const std::string& b) { return b.size(); };
}

bool LocalCache::put(const std::string& sha, std::string bytes, bool pinned) {
    if (!isSha256Hex(sha) || sha256Hex(bytes) != sha) return false;
    std::lock_guard<std::mutex> lock(m_);
    auto it = map_.find(sha);
    if (it != map_.end()) {
        lru_.splice(lru_.begin(), lru_, it->second.lru);
        it->second.pinned = it->second.pinned || pinned;
        return true;
    }
    const std::size_t size = sizeOf_(bytes);
    if (!pinned && size > capacity_) return false;
    lru_.push_front(sha);
    map_.emplace(sha, Entry{std::move(bytes), size, pinned, lru_.begin()});
    total_ += size;
    evictLocked();
    return true;
}

void LocalCache::evictLocked() {
    // Evict from the least-recent end, skipping pinned entries; the newest entry goes last.
    auto it = lru_.end();
    while (total_ > capacity_ && it != lru_.begin()) {
        --it;
        auto m = map_.find(*it);
        if (m->second.pinned) continue;
        total_ -= m->second.size;
        map_.erase(m);
        it = lru_.erase(it);
    }
}

std::optional<std::string> LocalCache::get(const std::string& sha) {
    std::lock_guard<std::mutex> lock(m_);
    auto it = map_.find(sha);
    if (it == map_.end()) return std::nullopt;
    if (sha256Hex(it->second.bytes) != sha) {  // corrupt entry: drop, never serve
        total_ -= it->second.size;
        lru_.erase(it->second.lru);
        map_.erase(it);
        return std::nullopt;
    }
    lru_.splice(lru_.begin(), lru_, it->second.lru);
    return it->second.bytes;
}

bool LocalCache::contains(const std::string& sha) const {
    std::lock_guard<std::mutex> lock(m_);
    return map_.count(sha) != 0;
}

bool LocalCache::setPinned(const std::string& sha, bool pinned) {
    std::lock_guard<std::mutex> lock(m_);
    auto it = map_.find(sha);
    if (it == map_.end()) return false;
    it->second.pinned = pinned;
    if (!pinned) evictLocked();
    return true;
}

bool LocalCache::erase(const std::string& sha) {
    std::lock_guard<std::mutex> lock(m_);
    auto it = map_.find(sha);
    if (it == map_.end()) return false;
    total_ -= it->second.size;
    lru_.erase(it->second.lru);
    map_.erase(it);
    return true;
}

std::size_t LocalCache::totalBytes() const { std::lock_guard<std::mutex> lock(m_); return total_; }
std::size_t LocalCache::entryCount() const { std::lock_guard<std::mutex> lock(m_); return map_.size(); }

}  // namespace cd::community
