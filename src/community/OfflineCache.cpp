#include "community/OfflineCache.h"
#include <algorithm>
#include <fstream>
#include <limits>
#include <vector>
#include "community/Sha256.h"
#include "state/StateDocument.h"

namespace fs = std::filesystem;

namespace cd::community {
namespace {
constexpr char kMagic[] = "CDC1\n";
constexpr std::size_t kHeader = 5 + 64 + 1;
bool isEntryName(const std::string& n) { return n.size() == 68 && n.compare(64, 4, ".cdc") == 0 && isSha256Hex(std::string_view(n).substr(0, 64)); }
// saveStateFileAtomic accepts UTF-8 on Windows as well as POSIX.
std::string pathUtf8(const fs::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}
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
    opened_ = false;
    if (cfg_.directory.empty()) { error = "cache directory not set"; return false; }
    std::error_code ec;
    fs::create_directories(cfg_.directory, ec);
    if (ec || !fs::is_directory(cfg_.directory, ec)) { error = "cannot create cache directory"; return false; }
    entries_.clear(); total_ = 0;
    struct Seen { std::string name; std::size_t size; fs::file_time_type t; };
    std::vector<Seen> seen;
    for (fs::directory_iterator it(cfg_.directory, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!fs::is_regular_file(it->symlink_status(e2)) || e2) continue;
        const std::string name = pathUtf8(it->path().filename());
        if (!isEntryName(name)) continue;
        const auto size = it->file_size(e2);
        if (e2) continue;
        if (size > std::numeric_limits<std::size_t>::max()) { error = "cache entry size cannot be represented"; return false; }
        const auto time = it->last_write_time(e2);
        if (e2) continue;
        seen.push_back({name, std::size_t(size), time});
    }
    if (ec) { error = "cannot scan cache directory"; return false; }
    std::sort(seen.begin(), seen.end(), [](const Seen& a, const Seen& b) { return a.t != b.t ? a.t < b.t : a.name < b.name; });
    std::size_t pinnedBytes = 0;
    for (auto& s : seen) {
        Entry e; e.size = s.size; e.lastUse = ++clock_;
        e.pinned = fs::is_regular_file(fs::symlink_status(cfg_.directory / (s.name + ".pin"), ec)) && !ec;
        if (e.size > std::numeric_limits<std::size_t>::max() - total_) {
            entries_.clear(); total_ = 0; error = "cache size cannot be represented"; return false;
        }
        if (e.pinned) {
            if (e.size > cfg_.maxBytes - pinnedBytes) {
                entries_.clear(); total_ = 0; error = "cache full of pinned entries"; return false;
            }
            pinnedBytes += e.size;
        }
        entries_[s.name] = e; total_ += e.size;
    }
    if (!evictLocked({}, 0, error)) { entries_.clear(); total_ = 0; return false; }
    opened_ = true;
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
    auto drop = [&] {
        std::error_code e;
        fs::remove(path, e);
        if (!e) { total_ -= std::min(total_, it->second.size); entries_.erase(it); }
        return CacheGet::Corrupt;
    };
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) return CacheGet::Miss;
    if (!fs::exists(status)) { total_ -= std::min(total_, it->second.size); entries_.erase(it); return CacheGet::Miss; }
    if (!fs::is_regular_file(status)) return drop();
    const auto size = fs::file_size(path, ec);
    if (ec) return CacheGet::Miss;
    if (size < kHeader || size - kHeader > cfg_.maxEntryBytes) return drop();
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
    if (cfg_.maxBytes < kHeader || bytes.size() > cfg_.maxBytes - kHeader) { error = "entry larger than the cache"; return false; }
    const auto newSize = bytes.size() + kHeader;
    // Refuse a pinned-capacity conflict before replacing the old entry or evicting anything.
    std::size_t pinnedBytes = 0;
    for (const auto& [key, entry] : entries_) {
        if (key == name || !entry.pinned) continue;
        if (entry.size > cfg_.maxBytes - pinnedBytes) { error = "cache full of pinned entries"; return false; }
        pinnedBytes += entry.size;
    }
    if (newSize > cfg_.maxBytes - pinnedBytes) { error = "cache full of pinned entries"; return false; }
    // Reserve room before committing the new file. A failed removal must not replace an
    // existing entry or leave a new entry on disk while accounting says it was evicted.
    if (!evictLocked(name, newSize, error)) return false;
    const fs::path dest = cfg_.directory / name;
    std::string raw;
    raw.reserve(newSize);
    raw.append(kMagic, 5);
    raw.append(sha256Hex(bytes));
    raw.push_back('\n');
    raw.append(bytes);
    if (!state::saveStateFileAtomic(pathUtf8(dest), raw, error)) return false;
    auto old = entries_.find(name);
    if (old != entries_.end()) total_ -= std::min(total_, old->second.size);
    Entry e; e.size = newSize; e.lastUse = ++clock_; e.pinned = old != entries_.end() && old->second.pinned;
    entries_[name] = e; total_ += e.size;
    error.clear();
    return true;
}

bool OfflineCache::evictLocked(const std::string& keep, std::size_t incomingBytes, std::string& error) {
    if (incomingBytes > cfg_.maxBytes) { error = "entry larger than the cache"; return false; }
    const auto kept = entries_.find(keep);
    const std::size_t keptBytes = kept == entries_.end() ? 0 : kept->second.size;
    const std::size_t allowance = cfg_.maxBytes - incomingBytes;
    if (total_ - keptBytes <= allowance) return true;
    // Attempt each eligible victim once, so one undeletable entry cannot block a later
    // removable victim or produce a retry loop. Only successful removals alter accounting.
    std::vector<std::string> victims;
    for (const auto& [name, entry] : entries_)
        if (!entry.pinned && name != keep) victims.push_back(name);
    std::sort(victims.begin(), victims.end(), [&](const auto& a, const auto& b) {
        return entries_.at(a).lastUse < entries_.at(b).lastUse;
    });
    for (const auto& name : victims) {
        if (total_ - keptBytes <= allowance) return true;
        auto victim = entries_.find(name);
        std::error_code e;
        fs::remove(cfg_.directory / name, e);
        if (e) continue;
        total_ -= victim->second.size;
        entries_.erase(victim);
    }
    if (total_ - keptBytes <= allowance) return true;
    error = "cannot remove enough unpinned cache entries";
    return false;
}

bool OfflineCache::remove(std::string_view id, std::string_view rev) {
    if (!isSafeIdentifier(id) || !isSafeIdentifier(rev)) return false;
    std::lock_guard<std::mutex> lock(m_);
    if (!opened_) return false;
    auto it = entries_.find(entryName(id, rev));
    if (it == entries_.end()) return false;
    std::error_code e;
    fs::remove(cfg_.directory / it->first, e);
    if (e) return false;
    fs::remove(cfg_.directory / (it->first + ".pin"), e);
    total_ -= std::min(total_, it->second.size);
    entries_.erase(it);
    return !e;
}

bool OfflineCache::setPinned(std::string_view id, std::string_view rev, bool pinned) {
    if (!isSafeIdentifier(id) || !isSafeIdentifier(rev)) return false;
    std::lock_guard<std::mutex> lock(m_);
    if (!opened_) return false;
    auto it = entries_.find(entryName(id, rev));
    if (it == entries_.end()) return false;
    const fs::path marker = cfg_.directory / (it->first + ".pin");
    std::error_code e;
    if (pinned) {
        std::string error;
        if (!state::saveStateFileAtomic(pathUtf8(marker), "\n", error)) return false;
    } else {
        const auto status = fs::symlink_status(marker, e);
        if (e && e != std::errc::no_such_file_or_directory) return false;
        if (fs::exists(status) && !fs::is_regular_file(status) && !fs::is_symlink(status)) return false;
        e.clear();
        fs::remove(marker, e);
        if (e) return false;
    }
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
