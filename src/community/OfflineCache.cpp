#include "community/OfflineCache.h"
#include <algorithm>
#include <fstream>
#include <vector>
#include "community/Sha256.h"

namespace fs = std::filesystem;

namespace cd::community {
namespace {
constexpr char kMagic[] = "CDC1\n";
constexpr std::size_t kHeader = 5 + 64 + 1;
bool isEntryName(const std::string& n) { return n.size() == 68 && n.compare(64, 4, ".cdc") == 0 && isSha256Hex(std::string_view(n).substr(0, 64)); }
}  // namespace

bool isSafeIdentifier(std::string_view s) noexcept {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    return true;
}

std::string OfflineCache::entryName(std::string_view id, std::string_view rev) {
    std::string key = "cd-cache-v1|";
    key.append(id); key.push_back('|'); key.append(rev);
    return sha256Hex(key) + ".cdc";
}

bool OfflineCache::open(std::string& error) {
    std::lock_guard<std::mutex> lock(m_);
    if (cfg_.directory.empty()) { error = "cache directory not set"; return false; }
    std::error_code ec;
    fs::create_directories(cfg_.directory, ec);
    if (ec || !fs::is_directory(cfg_.directory, ec)) { error = "cannot create cache directory"; return false; }
    entries_.clear(); total_ = 0;
    struct Seen { std::string name; std::size_t size; fs::file_time_type t; };
    std::vector<Seen> seen;
    for (fs::directory_iterator it(cfg_.directory, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2) || e2) continue;  // symlinks and directories are ignored
        const std::string name = it->path().filename().string();
        if (name.size() > 5 && name.compare(name.size() - 5, 5, ".part") == 0) { fs::remove(it->path(), e2); continue; }
        if (!isEntryName(name)) continue;
        const auto size = it->file_size(e2);
        if (e2) continue;
        seen.push_back({name, std::size_t(size), it->last_write_time(e2)});
    }
    std::sort(seen.begin(), seen.end(), [](const Seen& a, const Seen& b) { return a.t != b.t ? a.t < b.t : a.name < b.name; });
    for (auto& s : seen) {
        Entry e; e.size = s.size; e.lastUse = ++clock_;
        e.pinned = fs::exists(cfg_.directory / (s.name + ".pin"), ec);
        entries_[s.name] = e; total_ += e.size;
    }
    opened_ = true;
    evictLocked({});
    error.clear();
    return true;
}

CacheGet OfflineCache::get(std::string_view id, std::string_view rev, std::string& bytes) {
    if (!isSafeIdentifier(id) || !isSafeIdentifier(rev)) return CacheGet::InvalidKey;
    std::lock_guard<std::mutex> lock(m_);
    if (!opened_) return CacheGet::Miss;
    const std::string name = entryName(id, rev);
    auto it = entries_.find(name);
    if (it == entries_.end()) return CacheGet::Miss;
    const fs::path path = cfg_.directory / name;
    auto drop = [&] { std::error_code e; fs::remove(path, e); total_ -= std::min(total_, it->second.size); entries_.erase(it); return CacheGet::Corrupt; };
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) { total_ -= std::min(total_, it->second.size); entries_.erase(it); return CacheGet::Miss; }
    if (size < kHeader || size > cfg_.maxEntryBytes + kHeader) return drop();
    std::string raw(std::size_t(size), '\0');
    {
        std::ifstream in(path, std::ios::binary);
        if (!in.read(raw.data(), std::streamsize(raw.size()))) return drop();
    }
    if (raw.compare(0, 5, kMagic) != 0 || raw[kHeader - 1] != '\n') return drop();
    const std::string digest = raw.substr(5, 64);
    const std::string_view payload = std::string_view(raw).substr(kHeader);
    if (!isSha256Hex(digest) || sha256Hex(payload) != digest) return drop();
    bytes.assign(payload);
    it->second.lastUse = ++clock_;
    fs::last_write_time(path, fs::file_time_type::clock::now(), ec);  // persist recency across restarts
    return CacheGet::Hit;
}

bool OfflineCache::put(std::string_view id, std::string_view rev, std::string_view bytes, std::string& error) {
    if (!isSafeIdentifier(id) || !isSafeIdentifier(rev)) { error = "invalid cache key"; return false; }
    if (bytes.empty() || bytes.size() > cfg_.maxEntryBytes) { error = "entry exceeds the per-entry bound"; return false; }
    std::lock_guard<std::mutex> lock(m_);
    if (!opened_) { error = "cache not open"; return false; }
    const std::string name = entryName(id, rev);
    if (bytes.size() + kHeader > cfg_.maxBytes) { error = "entry larger than the cache"; return false; }
    const fs::path dest = cfg_.directory / name;
    const fs::path tmp = cfg_.directory / (name + "." + std::to_string(++tmpCounter_) + ".part");
    std::error_code ec;
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!out) { error = "cannot create temporary cache file"; return false; }
        out.write(kMagic, 5);
        const std::string digest = sha256Hex(bytes);
        out.write(digest.data(), std::streamsize(digest.size()));
        out.put('\n');
        out.write(bytes.data(), std::streamsize(bytes.size()));
        out.flush();
        if (!out) { out.close(); fs::remove(tmp, ec); error = "write failed (disk full?)"; return false; }
        out.close();
        if (out.fail()) { fs::remove(tmp, ec); error = "close failed"; return false; }
    }
    fs::rename(tmp, dest, ec);  // atomic replace on the same directory
    if (ec) { std::error_code e2; fs::remove(tmp, e2); error = "rename failed"; return false; }
    auto old = entries_.find(name);
    if (old != entries_.end()) total_ -= std::min(total_, old->second.size);
    Entry e; e.size = bytes.size() + kHeader; e.lastUse = ++clock_; e.pinned = old != entries_.end() && old->second.pinned;
    entries_[name] = e; total_ += e.size;
    evictLocked(name);
    if (!entries_.count(name)) { error = "cache full of pinned entries"; return false; }
    error.clear();
    return true;
}

void OfflineCache::evictLocked(const std::string& keep) {
    while (total_ > cfg_.maxBytes) {
        auto victim = entries_.end();
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->second.pinned || it->first == keep) continue;
            if (victim == entries_.end() || it->second.lastUse < victim->second.lastUse) victim = it;
        }
        if (victim == entries_.end()) {
            // Only pinned entries and the new one remain: reject the new one rather than exceed the cap.
            auto k = entries_.find(keep);
            if (k != entries_.end() && !k->second.pinned) {
                std::error_code e; fs::remove(cfg_.directory / k->first, e);
                total_ -= std::min(total_, k->second.size); entries_.erase(k);
            }
            return;
        }
        std::error_code e;
        fs::remove(cfg_.directory / victim->first, e);
        total_ -= std::min(total_, victim->second.size);
        entries_.erase(victim);
    }
}

bool OfflineCache::remove(std::string_view id, std::string_view rev) {
    if (!isSafeIdentifier(id) || !isSafeIdentifier(rev)) return false;
    std::lock_guard<std::mutex> lock(m_);
    auto it = entries_.find(entryName(id, rev));
    if (it == entries_.end()) return false;
    std::error_code e;
    fs::remove(cfg_.directory / it->first, e);
    fs::remove(cfg_.directory / (it->first + ".pin"), e);
    total_ -= std::min(total_, it->second.size);
    entries_.erase(it);
    return true;
}

bool OfflineCache::setPinned(std::string_view id, std::string_view rev, bool pinned) {
    if (!isSafeIdentifier(id) || !isSafeIdentifier(rev)) return false;
    std::lock_guard<std::mutex> lock(m_);
    auto it = entries_.find(entryName(id, rev));
    if (it == entries_.end()) return false;
    const fs::path marker = cfg_.directory / (it->first + ".pin");
    std::error_code e;
    if (pinned) { std::ofstream(marker, std::ios::binary).put('\n'); } else fs::remove(marker, e);
    it->second.pinned = pinned;
    return true;
}

std::size_t OfflineCache::totalBytes() const { std::lock_guard<std::mutex> lock(m_); return total_; }
std::size_t OfflineCache::entryCount() const { std::lock_guard<std::mutex> lock(m_); return entries_.size(); }
bool OfflineCache::contains(std::string_view id, std::string_view rev) const {
    if (!isSafeIdentifier(id) || !isSafeIdentifier(rev)) return false;
    std::lock_guard<std::mutex> lock(m_);
    return entries_.count(entryName(id, rev)) != 0;
}

}  // namespace cd::community
