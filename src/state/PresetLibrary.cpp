#include "state/PresetLibrary.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

namespace cd::state {
namespace fs = std::filesystem;
namespace {
std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = char(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

std::string utf8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

bool endsWith(std::string_view s, std::string_view suf) { return s.size() >= suf.size() && s.substr(s.size() - suf.size()) == suf; }

// Reads a regular file of at most `maxBytes` bytes. The initial size is also the
// read limit: a growing file cannot turn the size check into an unbounded read.
bool readBounded(const fs::path& p, std::size_t maxBytes, std::string& out, std::string& error,
                 std::size_t* admittedBytes = nullptr) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec) || ec) { error = "not a regular file"; return false; }
    const auto size = fs::file_size(p, ec);
    if (ec) { error = "cannot stat file"; return false; }
    if (size > maxBytes) { error = "file exceeds the read size bound"; return false; }
    std::ifstream f(p, std::ios::binary);
    if (!f) { error = "cannot open file"; return false; }
    if (admittedBytes) *admittedBytes = std::size_t(size);
    std::string candidate(std::size_t(size), '\0');
    if (size != 0 && !f.read(candidate.data(), std::streamsize(candidate.size()))) {
        error = "read failed or file changed"; return false;
    }
    if (f.peek() != std::char_traits<char>::eof() || f.bad()) {
        error = "read failed or file changed"; return false;
    }
    out = std::move(candidate);
    return true;
}

bool matches(const PresetInfo& i, const std::string& q) {
    if (q.empty()) return true;
    if (lower(i.id).find(q) != std::string::npos || lower(i.title).find(q) != std::string::npos ||
        lower(i.author).find(q) != std::string::npos) return true;
    for (const auto& t : i.tags) if (lower(t).find(q) != std::string::npos) return true;
    return false;
}

PresetInfo infoFrom(std::string id, const ScenePreset& p, bool factory) {
    PresetInfo i;
    i.id = std::move(id);
    i.title = p.meta.title; i.author = p.meta.author; i.tags = p.meta.tags;
    i.factory = factory;
    return i;
}
}  // namespace

bool sanitizeStem(std::string_view n, std::string& error) {
    if (n.empty()) { error = "empty name"; return false; }
    if (n.size() > kMaxStemBytes) { error = "name too long"; return false; }
    for (unsigned char c : n) {
        const bool ok = std::isalnum(c) && c < 0x80 ? true : (c == ' ' || c == '-' || c == '_' || c == '.' || c == '(' || c == ')');
        if (!ok) { error = "name contains a character that is not allowed (letters, digits, space, - _ . ( ) only)"; return false; }
    }
    if (n.front() == '.' || n.front() == ' ' || n.back() == '.' || n.back() == ' ') { error = "name must not start/end with a dot or space"; return false; }
    if (n.find("..") != std::string_view::npos) { error = "name must not contain '..'"; return false; }
    std::string base = lower(n.substr(0, n.find('.')));
    while (!base.empty() && base.back() == ' ') base.pop_back();  // "CON .txt" is reserved on Windows too
    static const char* const reserved[] = {"con", "prn", "aux", "nul"};
    bool res = std::any_of(std::begin(reserved), std::end(reserved), [&](const char* r) { return base == r; });
    if (!res && base.size() == 4 && (base.compare(0, 3, "com") == 0 || base.compare(0, 3, "lpt") == 0) && base[3] >= '1' && base[3] <= '9') res = true;
    if (res) { error = "reserved device name"; return false; }
    error.clear();
    return true;
}

fs::path PresetLibrary::pathFor(const std::string& stem) const {
    return dir_ / (stem + std::string(kPresetExtension));
}

std::vector<PresetInfo> PresetLibrary::list(std::string_view query) const {
    return filterPresets(scan(), query);
}

std::vector<PresetInfo> filterPresets(const PresetCatalog& catalog, std::string_view query) {
    const std::string q = lower(query);
    std::vector<PresetInfo> out;
    for (const auto& entry : catalog.entries) if (matches(entry, q)) out.push_back(entry);
    return out;
}

PresetCatalog PresetLibrary::scan(PresetScanLimits limits) const {
    limits.entries = std::min(limits.entries, kMaxListedPresets);
    limits.bytes = std::min(limits.bytes, kMaxCatalogReadBytes);
    PresetCatalog catalog;
    for (const auto& f : factoryPresets()) {
        catalog.entries.push_back(infoFrom("factory:" + f.id, f.scene, true));
    }
    std::vector<PresetInfo> user;
    std::error_code ec;
    for (fs::directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec)) {
        if (catalog.visited == limits.entries) { catalog.truncated = true; break; }
        ++catalog.visited;  // includes irrelevant and nonmatching entries
        const std::string name = utf8(it->path().filename());
        if (!endsWith(name, kPresetExtension)) continue;
        std::error_code sec;
        if (!it->is_regular_file(sec) || sec) continue;
        const std::string stem = name.substr(0, name.size() - kPresetExtension.size());
        std::string why;
        if (!sanitizeStem(stem, why)) continue;
        PresetInfo i;
        i.id = "user:" + stem;
        i.title = stem;
        const auto size = it->file_size(sec);
        if (!sec && size <= kSceneMaxBytes && size > limits.bytes - catalog.bytesRead) {
            catalog.truncated = true;
            break;
        }
        std::string text;
        ScenePreset p;
        // Recheck the remaining aggregate budget in readBounded as well: a
        // file may grow between the directory-entry size and the actual read.
        const auto remaining = std::min(kSceneMaxBytes, limits.bytes - catalog.bytesRead);
        std::size_t admittedBytes = 0;
        const bool read = readBounded(it->path(), remaining, text, why, &admittedBytes);
        catalog.bytesRead += admittedBytes;  // failed/partial reads consume budget too
        if (read && sceneFromJson(text, p, why)) i = infoFrom(i.id, p, false);
        else { i.valid = false; i.error = why; }
        user.push_back(std::move(i));
    }
    std::sort(user.begin(), user.end(), [](const PresetInfo& a, const PresetInfo& b) { return a.id < b.id; });
    catalog.entries.insert(catalog.entries.end(), std::make_move_iterator(user.begin()), std::make_move_iterator(user.end()));
    return catalog;
}

bool PresetLibrary::load(const std::string& id, ScenePreset& out, std::string& error) const {
    if (id.rfind("factory:", 0) == 0) {
        const std::string slug = id.substr(8);
        for (const auto& f : factoryPresets()) if (f.id == slug) { out = f.scene; error.clear(); return true; }
        error = "unknown factory preset";
        return false;
    }
    if (id.rfind("user:", 0) != 0) { error = "unknown preset id"; return false; }
    const std::string stem = id.substr(5);
    if (!sanitizeStem(stem, error)) return false;
    std::string text;
    if (!readBounded(pathFor(stem), kSceneMaxBytes, text, error)) return false;
    return sceneFromJson(text, out, error);
}

bool PresetLibrary::save(std::string_view name, const ScenePreset& preset, bool overwrite, std::string& savedId, std::string& error) const {
    if (!sanitizeStem(name, error)) return false;
    const std::string stem(name);
    std::string text;
    if (!sceneToJson(preset, text, error)) return false;
    std::error_code ec;
    const fs::path target = pathFor(stem);
    if (!overwrite && fs::exists(fs::symlink_status(target, ec))) { error = "preset already exists"; return false; }
    fs::create_directories(dir_, ec);
    if (ec) { error = "cannot create preset directory"; return false; }
    // The earlier exists() check provides a friendly error only. Exclusivity
    // must be enforced at the final filesystem operation, not before it.
    if (!(overwrite ? saveStateFileAtomic(utf8(target), text, error)
                    : saveStateFileAtomicNoReplace(utf8(target), text, error))) return false;
    savedId = "user:" + stem;
    error.clear();
    return true;
}

bool PresetLibrary::remove(const std::string& id, std::string& error) const {
    if (id.rfind("user:", 0) != 0) { error = "only user presets can be removed"; return false; }
    const std::string stem = id.substr(5);
    if (!sanitizeStem(stem, error)) return false;
    std::error_code ec;
    if (!fs::remove(pathFor(stem), ec) || ec) { error = "preset not found or not removable"; return false; }
    error.clear();
    return true;
}

bool PresetLibrary::importFile(const fs::path& source, std::string_view name, bool overwrite, std::string& savedId, std::string& error) const {
    std::string text;
    if (!readBounded(source, kSceneMaxBytes, text, error)) return false;
    ScenePreset p;
    if (!sceneFromJson(text, p, error)) return false;
    std::string stem(name);
    if (stem.empty()) {
        stem = utf8(source.filename());
        if (endsWith(stem, kPresetExtension)) stem.resize(stem.size() - kPresetExtension.size());
        else stem = utf8(source.stem());
    }
    return save(stem, p, overwrite, savedId, error);
}

bool PresetLibrary::exportPreset(const std::string& id, const fs::path& destination, std::string& error) const {
    ScenePreset p;
    if (!load(id, p, error)) return false;
    std::string text;
    if (!sceneToJson(p, text, error)) return false;
    if (destination.empty()) { error = "empty destination"; return false; }
    return saveStateFileAtomic(utf8(destination), text, error);
}

}  // namespace cd::state
