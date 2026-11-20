// CircuitDrift state: local preset library (factory + user), import/export. Worker/message thread
// only (allocates, does file I/O); never call from audio. No path is hardcoded: the user directory is
// injected. Offline: no network access of any kind.
//
// Ids: "factory:<slug>" (compiled in, read-only) and "user:<stem>" (file <stem>.circuitdrift.json in
// the injected directory). File names are derived ONLY from names that pass sanitizeStem(); anything
// else is rejected, never rewritten, so a name can never escape the directory.
#pragma once
#include <cstddef>
#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>
#include "state/Preset.h"

namespace cd::state {

inline constexpr std::size_t kMaxStemBytes = 80;
inline constexpr std::size_t kMaxListedPresets = 5000;
inline constexpr std::size_t kMaxCatalogReadBytes = 32 * 1024 * 1024;
inline constexpr std::string_view kPresetExtension = ".circuitdrift.json";

// Accepts ASCII letters, digits, space, '-', '_', '.', '(' and ')'; 1..kMaxStemBytes bytes; no leading
// or trailing space/dot, no ".." anywhere; not a Windows reserved device name (CON, PRN, AUX, NUL,
// COM1-9, LPT1-9, optionally followed by '.'+extension, case-insensitive). Path separators, drive
// colons, control characters and everything else are rejected.
bool sanitizeStem(std::string_view name, std::string& error);

struct PresetInfo {
    std::string id;  // "factory:clean-console" or "user:My Preset"
    std::string title, author;
    std::vector<std::string> tags;
    bool factory = false;
    bool valid = true;  // false: user file present but unreadable/invalid (error says why)
    std::string error;
};

struct PresetScanLimits {
    std::size_t entries = kMaxListedPresets;
    std::size_t bytes = kMaxCatalogReadBytes;
};
struct PresetCatalog {
    std::vector<PresetInfo> entries;
    std::size_t visited = 0, bytesRead = 0;
    bool truncated = false;
    bool cancelled = false;
    std::string error;  // directory-wide failure; individual invalid files keep their own error
};
// Filtering a catalog does no filesystem I/O. Factory order and user id order
// are preserved. Reuse a snapshot while typing rather than rescanning the disk.
std::vector<PresetInfo> filterPresets(const PresetCatalog&, std::string_view query);

class PresetLibrary {
public:
    explicit PresetLibrary(std::filesystem::path userDirectory) : dir_(std::move(userDirectory)) {}
    const std::filesystem::path& userDirectory() const noexcept { return dir_; }

    // Factory first (compiled order) then user presets sorted by id. `query` filters by
    // case-insensitive substring over id, title, author and tags (empty = all).
    std::vector<PresetInfo> list(std::string_view query = {}) const;
    // Work is bounded independently of search matches. Limits can be reduced
    // by callers, never raised above the library's hard entry/byte budgets.
    // Cancellation is checked between filesystem operations and bounded reads/parses;
    // it cannot interrupt an operating-system call already in progress.
    PresetCatalog scan(PresetScanLimits limits = {}, std::stop_token cancellation = {}) const;
    bool load(const std::string& id, ScenePreset& out, std::string& error) const;
    // Saves under `name` (must pass sanitizeStem). Fails if it exists and !overwrite. Atomic.
    bool save(std::string_view name, const ScenePreset& preset, bool overwrite, std::string& savedId, std::string& error) const;
    bool remove(const std::string& id, std::string& error) const;  // user presets only
    // Reads, strictly validates and re-saves canonically into the library. `name` empty = derive from
    // the source file stem. Size bound kSceneMaxBytes enforced before reading.
    bool importFile(const std::filesystem::path& source, std::string_view name, bool overwrite, std::string& savedId, std::string& error) const;
    // Writes the canonical JSON of preset `id` to `destination` (user-chosen path, atomic).
    bool exportPreset(const std::string& id, const std::filesystem::path& destination, std::string& error) const;

private:
    std::filesystem::path pathFor(const std::string& stem) const;
    std::filesystem::path dir_;
};

}  // namespace cd::state
