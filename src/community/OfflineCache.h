// CircuitDrift community: bounded offline cache of downloaded scene bytes (CD-F04, spec: 250 MiB LRU).
// - Directory is injected; entry file names are SHA-256 of "cd-cache-v1|id|rev" (64 hex + ".cdc"), so ids
//   and revisions can never form a path (traversal-safe); ids/revisions are additionally validated.
// - File format: "CDC1\n" + sha256(payload) hex + "\n" + payload. Read recomputes the digest and
//   deletes entries that fail (Corrupt). Content is still fully re-validated by the client before use.
// - Writes go to an exclusively named temp file in the same directory, are flushed and closed, then
//   renamed over the destination (atomic replace); failed writes remove the temp file.
// - LRU by access order (in memory, seeded from file mtimes at open); unpinned entries are evicted
//   first-in-LRU until the cap holds. Pins are persisted as "<name>.pin" marker files.
// - Imported presets are embedded in project state independently; this cache is only a convenience.
// Threading: worker only (file I/O); internally synchronised.
#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace cd::community {

constexpr std::size_t kDefaultCacheBytes = 250u * 1024u * 1024u;

struct CacheConfig {
    std::filesystem::path directory;
    std::size_t maxBytes = kDefaultCacheBytes;
    std::size_t maxEntryBytes = 1310720;  // scene bound (1.25 MiB)
};
enum class CacheGet : unsigned char { Hit, Miss, Corrupt, InvalidKey };

// [A-Za-z0-9_-]{1,64}
bool isSafeIdentifier(std::string_view s) noexcept;

class OfflineCache {
public:
    explicit OfflineCache(CacheConfig cfg) : cfg_(std::move(cfg)) {}
    bool open(std::string& error);  // creates the directory, removes stale temp files, scans entries
    CacheGet get(std::string_view id, std::string_view rev, std::string& bytes);
    bool put(std::string_view id, std::string_view rev, std::string_view bytes, std::string& error);
    bool remove(std::string_view id, std::string_view rev);
    bool setPinned(std::string_view id, std::string_view rev, bool pinned);
    std::size_t totalBytes() const;
    std::size_t entryCount() const;
    bool contains(std::string_view id, std::string_view rev) const;
    static std::string entryName(std::string_view id, std::string_view rev);  // "<hex>.cdc"

private:
    struct Entry { std::size_t size = 0; std::uint64_t lastUse = 0; bool pinned = false; };
    void evictLocked(const std::string& keep);
    CacheConfig cfg_;
    bool opened_ = false;
    mutable std::mutex m_;
    std::map<std::string, Entry> entries_;
    std::size_t total_ = 0;
    std::uint64_t clock_ = 0, tmpCounter_ = 0;
};

}  // namespace cd::community
