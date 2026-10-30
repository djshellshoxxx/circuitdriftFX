#include "state/StateDocument.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <stdexcept>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <limits>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
#include "core/ParameterRegistry.h"
#include "dsp/DeviceManifest.h"

namespace cd::state {
namespace {
using core::JsonValue;

constexpr double kMaxSafeInt = 9007199254740992.0;  // 2^53

bool isInt(const JsonValue* v, double lo, double hi) {
    return v && v->isNumber() && v->numberIsExactInteger && std::isfinite(v->number)
        && v->number == std::floor(v->number) && v->number >= lo && v->number <= hi;
}
bool str(const JsonValue* v, std::size_t maxLen, std::string& out, bool allowEmpty = true) {
    if (!v || !v->isString() || v->string.size() > maxLen || (!allowEmpty && v->string.empty())) return false;
    out = v->string;
    return true;
}
bool strList(const JsonValue* v, std::size_t maxItems, std::size_t maxLen, std::vector<std::string>& out) {
    if (!v || !v->isArray() || v->array.size() > maxItems) return false;
    out.clear();
    for (const auto& e : v->array) {
        if (!e.isString() || e.string.empty() || e.string.size() > maxLen) return false;
        out.push_back(e.string);
    }
    return true;
}
// RFC 3339 UTC subset: YYYY-MM-DDTHH:MM:SSZ (optionally with .fraction before Z).
bool rfc3339(const std::string& s) {
    if (s.size() < 20 || s.back() != 'Z') return false;
    const char* pat = "dddd-dd-ddTdd:dd:dd";
    for (int i = 0; pat[i]; ++i)
        if (pat[i] == 'd' ? !(s[i] >= '0' && s[i] <= '9') : s[i] != pat[i]) return false;
    if (s.size() > 20) {
        if (s[19] != '.' || s.size() < 22) return false;
        for (std::size_t i = 20; i + 1 < s.size(); ++i) if (!(s[i] >= '0' && s[i] <= '9')) return false;
    }
    return true;
}

bool validBlueprint(const JsonValue& b, std::string& why) {
    if (!b.isObject()) { why = "blueprint not an object"; return false; }
    if (core::writeJson(b).size() > kBlueprintMaxBytes) { why = "blueprint exceeds 1 MiB"; return false; }
    if (!isInt(b.get("schemaVersion"), 1, 1)) { why = "blueprint schemaVersion"; return false; }
    std::string id;
    if (!str(b.get("id"), 128, id, false)) { why = "blueprint id"; return false; }
    const JsonValue* comps = b.get("components");
    if (!comps || !comps->isArray() || comps->array.size() > 64) { why = "blueprint components"; return false; }
    for (const auto& c : comps->array) {
        const JsonValue* m = c.isObject() ? c.get("modelId") : nullptr;
        // Only the synthetic fixture catalogue exists until G01; anything else is an unknown model.
        if (!m || !m->isString() || !dsp::findSyntheticFixture(m->string)) { why = "unknown model in blueprint"; return false; }
    }
    return true;
}

bool parseCheckpoint(const JsonValue& v, EngineCheckpoint& c, std::string& why) {
    static const std::set<std::string> keys = {"engineModelVersion", "modelRevision", "sampleRate", "oversampling",
                                               "sampleIndex", "electrical", "thermal", "rng"};
    if (!v.isObject()) { why = "checkpoint not an object"; return false; }
    for (const auto& kv : v.object) if (!keys.count(kv.first)) { why = "unknown checkpoint key " + kv.first; return false; }
    if (!str(v.get("engineModelVersion"), 64, c.engineModelVersion, false) || !str(v.get("modelRevision"), 64, c.modelRevision, false)) {
        why = "checkpoint identity"; return false;
    }
    const JsonValue* fs = v.get("sampleRate");
    if (!fs || !fs->isNumber() || !(fs->number >= 8000 && fs->number <= 768000)) { why = "checkpoint sampleRate"; return false; }
    c.sampleRate = fs->number;
    const JsonValue* os = v.get("oversampling");
    if (!isInt(os, 1, 16) || (int(os->number) & (int(os->number) - 1))) { why = "checkpoint oversampling"; return false; }
    c.oversampling = int(os->number);
    if (!isInt(v.get("sampleIndex"), 0, kMaxSafeInt)) { why = "checkpoint sampleIndex"; return false; }
    c.sampleIndex = std::uint64_t(v.get("sampleIndex")->number);
    for (auto [name, dst] : {std::pair<const char*, std::vector<double>*>{"electrical", &c.electrical}, {"thermal", &c.thermal}}) {
        const JsonValue* a = v.get(name);
        if (!a || !a->isArray() || a->array.size() > 4096) { why = std::string("checkpoint ") + name; return false; }
        dst->clear();
        for (const auto& e : a->array) {
            if (!e.isNumber() || !std::isfinite(e.number)) { why = std::string("checkpoint ") + name + " value"; return false; }
            dst->push_back(e.number);
        }
    }
    const JsonValue* r = v.get("rng");
    if (!r || !r->isArray() || r->array.size() > 64) { why = "checkpoint rng"; return false; }
    c.rng.clear();
    for (const auto& e : r->array) {  // 64-bit states as exactly 16 lowercase hex digits
        if (!e.isString() || e.string.size() != 16 || e.string.find_first_not_of("0123456789abcdef") != std::string::npos) {
            why = "checkpoint rng state"; return false;
        }
        c.rng.push_back(std::strtoull(e.string.c_str(), nullptr, 16));
    }
    return true;
}

JsonValue checkpointJson(const EngineCheckpoint& c) {
    JsonValue o = JsonValue::makeObject();
    o.set("engineModelVersion", JsonValue::makeString(c.engineModelVersion));
    o.set("modelRevision", JsonValue::makeString(c.modelRevision));
    o.set("sampleRate", JsonValue::makeNumber(c.sampleRate));
    o.set("oversampling", JsonValue::makeNumber(c.oversampling));
    o.set("sampleIndex", JsonValue::makeNumber(double(c.sampleIndex)));
    for (auto [name, src] : {std::pair<const char*, const std::vector<double>*>{"electrical", &c.electrical}, {"thermal", &c.thermal}}) {
        JsonValue a = JsonValue::makeArray();
        for (double d : *src) a.array.push_back(JsonValue::makeNumber(d));
        o.set(name, std::move(a));
    }
    JsonValue r = JsonValue::makeArray();
    for (std::uint64_t s : c.rng) {
        char b[24];
        std::snprintf(b, sizeof b, "%016llx", (unsigned long long)s);
        r.array.push_back(JsonValue::makeString(b));
    }
    o.set("rng", std::move(r));
    return o;
}

const std::set<std::string>& knownKeys() {
    static const std::set<std::string> k = {"schemaVersion", "product", "kind", "engineModelVersion", "minReaderVersion",
                                            "author", "title", "tags", "createdAt", "requiredFeatures", "blueprint",
                                            "blueprints", "baseParameters", "macros", "timeline", "faults", "seed", "ui", "checkpoint"};
    return k;
}

// Feature identity is independent of whether the feature exposes a host parameter.
// Recognition is not a claim that a feature is implemented or runtime-qualified.
constexpr std::array<std::string_view, 23> kKnownFeatures = {
    "CD-F03", "CD-F04", "CD-F05", "CD-F06", "CD-F07", "CD-F08", "CD-F09", "CD-F10",
    "CD-F11", "CD-F12", "CD-F13", "CD-F14", "CD-F15", "CD-F16", "CD-F17", "CD-F18",
    "CD-F19", "CD-F20", "CD-F21", "CD-F22", "CD-F23", "CD-F24", "CD-F25"
};

bool validateV1(const JsonValue& v, StateDocument& d, std::string& why) {
    if (!v.isObject()) { why = "document not an object"; return false; }
    const JsonValue* p = v.get("product");
    if (!p || !p->isString() || p->string != "CircuitDrift") { why = "product must be CircuitDrift"; return false; }
    const JsonValue* k = v.get("kind");
    if (!k || !k->isString() || (k->string != "scene" && k->string != "session")) { why = "kind must be scene or session"; return false; }
    d.kind = k->string == "scene" ? DocKind::Scene : DocKind::Session;
    if (!str(v.get("engineModelVersion"), 64, d.engineModelVersion, false)) { why = "engineModelVersion"; return false; }
    if (d.engineModelVersion != kEngineModelVersion) { why = "unsupported engineModelVersion"; return false; }
    if (!isInt(v.get("minReaderVersion"), 1, 1e6)) { why = "minReaderVersion"; return false; }
    d.minReaderVersion = int(v.get("minReaderVersion")->number);
    if (d.minReaderVersion > kReaderVersion) { why = "document requires a newer reader"; return false; }
    if (!str(v.get("author"), 256, d.author) || !str(v.get("title"), 256, d.title)) { why = "author/title"; return false; }
    if (!strList(v.get("tags"), 32, 64, d.tags)) { why = "tags"; return false; }
    if (!str(v.get("createdAt"), 40, d.createdAt) || !rfc3339(d.createdAt)) { why = "createdAt must be RFC 3339 UTC"; return false; }
    if (!strList(v.get("requiredFeatures"), 64, 16, d.requiredFeatures)) { why = "requiredFeatures"; return false; }
    for (const auto& f : d.requiredFeatures) {
        if (std::find(kKnownFeatures.begin(), kKnownFeatures.end(), f) == kKnownFeatures.end()) {
            why = "unknown required feature " + f; return false;
        }
    }
    d.blueprints.clear();
    if (d.kind == DocKind::Scene) {
        if (v.get("blueprints") || !v.get("blueprint")) { why = "scene needs exactly one 'blueprint'"; return false; }
        d.blueprints.push_back(*v.get("blueprint"));
    } else {
        const JsonValue* bs = v.get("blueprints");
        if (v.get("blueprint") || !bs || !bs->isArray() || bs->array.empty() || bs->array.size() > 2) {
            why = "session needs 'blueprints' with 1..2 entries"; return false;
        }
        d.blueprints = bs->array;
    }
    for (const auto& b : d.blueprints) if (!validBlueprint(b, why)) return false;

    const JsonValue* bp = v.get("baseParameters");
    if (!bp || !bp->isObject()) { why = "baseParameters"; return false; }
    d.baseParameters.clear();
    for (const auto& kv : bp->object) {
        if (kv.first.find('\0') != std::string::npos) { why = "NUL in parameter identity"; return false; }
        const core::ParamSpec* spec = core::findById(kv.first.c_str());
        if (!spec) { why = "unknown parameter " + kv.first; return false; }
        if (!kv.second.isNumber()) { why = "parameter not a number: " + kv.first; return false; }
        const double x = kv.second.number;
        if (!std::isfinite(x) || x < spec->min || x > spec->max) { why = "parameter out of range (not clamped): " + kv.first; return false; }
        if (spec->kind != core::ParamKind::Float && !isInt(&kv.second, spec->min, spec->max)) { why = "non-integral discrete parameter: " + kv.first; return false; }
        d.baseParameters.emplace_back(kv.first, x);
    }
    const JsonValue* mac = v.get("macros");
    if (!mac || !mac->isObject()) { why = "macros must be an object"; return false; }
    d.macros = *mac;
    for (auto [name, dst] : {std::pair<const char*, JsonValue*>{"timeline", &d.timeline}, {"faults", &d.faults}}) {
        const JsonValue* t = v.get(name);
        if (!t || !(t->isObject() || t->isArray())) { why = std::string(name) + " must be an object or array"; return false; }
        *dst = *t;
    }
    if (!isInt(v.get("seed"), 0, kMaxSafeInt)) { why = "seed must be an integer in [0, 2^53]"; return false; }
    d.seed = std::uint64_t(v.get("seed")->number);
    d.ui.reset();
    if (const JsonValue* u = v.get("ui")) {
        if (!u->isObject()) { why = "ui must be an object"; return false; }
        d.ui = *u;
    }
    d.checkpoint.reset();
    if (const JsonValue* c = v.get("checkpoint")) {
        if (d.kind != DocKind::Session) { why = "checkpoint only allowed in sessions"; return false; }
        EngineCheckpoint cp;
        if (!parseCheckpoint(*c, cp, why)) return false;
        d.checkpoint = std::move(cp);
    }
    d.metadata.clear();
    std::size_t metaBytes = 0;
    for (const auto& kv : v.object) {
        if (knownKeys().count(kv.first)) continue;
        metaBytes += kv.first.size() + core::writeJson(kv.second).size();
        if (metaBytes > kMetadataMaxBytes) { why = "unknown metadata exceeds bound"; return false; }
        d.metadata.push_back(kv);
    }
    return true;
}

JsonValue toJson(const StateDocument& d, bool forPublish) {
    if (d.seed > 9007199254740992ULL || (!forPublish && d.checkpoint && d.checkpoint->sampleIndex > 9007199254740992ULL))
        throw std::invalid_argument("schema v1 numeric identities must be in [0, 2^53]");
    JsonValue o = JsonValue::makeObject();
    o.set("schemaVersion", JsonValue::makeNumber(kSchemaVersion));
    o.set("product", JsonValue::makeString("CircuitDrift"));
    o.set("kind", JsonValue::makeString(d.kind == DocKind::Scene ? "scene" : "session"));
    o.set("engineModelVersion", JsonValue::makeString(d.engineModelVersion));
    o.set("minReaderVersion", JsonValue::makeNumber(d.minReaderVersion));
    o.set("author", JsonValue::makeString(d.author));
    o.set("title", JsonValue::makeString(d.title));
    for (auto [name, list] : {std::pair<const char*, const std::vector<std::string>*>{"tags", &d.tags}, {"requiredFeatures", &d.requiredFeatures}}) {
        JsonValue a = JsonValue::makeArray();
        for (const auto& s : *list) a.array.push_back(JsonValue::makeString(s));
        o.set(name, std::move(a));
    }
    o.set("createdAt", JsonValue::makeString(d.createdAt));
    if (d.kind == DocKind::Scene) {
        o.set("blueprint", d.blueprints.empty() ? JsonValue::makeObject() : d.blueprints[0]);
    } else {
        JsonValue a = JsonValue::makeArray();
        a.array = d.blueprints;
        o.set("blueprints", std::move(a));
    }
    JsonValue bp = JsonValue::makeObject();
    for (const auto& kv : d.baseParameters) bp.set(kv.first, JsonValue::makeNumber(kv.second));
    o.set("baseParameters", std::move(bp));
    o.set("macros", d.macros.isObject() ? d.macros : JsonValue::makeObject());
    o.set("timeline", (d.timeline.isObject() || d.timeline.isArray()) ? d.timeline : JsonValue::makeObject());
    o.set("faults", (d.faults.isObject() || d.faults.isArray()) ? d.faults : JsonValue::makeObject());
    o.set("seed", JsonValue::makeNumber(double(d.seed)));
    if (!forPublish) {
        if (d.ui) o.set("ui", *d.ui);
        if (d.checkpoint) o.set("checkpoint", checkpointJson(*d.checkpoint));
        for (const auto& kv : d.metadata) o.set(kv.first, kv.second);
    }
    return o;
}

bool noMigration(JsonValue&, std::string&) { return false; }

bool publicIdentifier(const JsonValue* v) {
    if (!v || !v->isString() || v->string.empty() || v->string.size() > 64) return false;
    return v->string.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-") == std::string::npos;
}

bool publishableContent(const StateDocument& d, std::string& error) {
    auto emptyContainer = [](const JsonValue& v) {
        return (v.isObject() && v.object.empty()) || (v.isArray() && v.array.empty());
    };
    if (!d.macros.isObject() || !d.macros.object.empty() || !emptyContainer(d.timeline) || !emptyContainer(d.faults)) {
        error = "publication requires empty macro/timeline/fault payloads until their semantic validators are available";
        return false;
    }
    if (d.blueprints.size() != 1) { error = "publication requires exactly one blueprint"; return false; }
    const JsonValue& b = d.blueprints[0];
    const JsonValue* components = b.get("components");
    if (!b.isObject() || b.object.size() != 3 || !isInt(b.get("schemaVersion"), 1, 1)
        || !publicIdentifier(b.get("id")) || !components || !components->isArray()) {
        error = "publication only supports the minimal synthetic blueprint; opaque fields require P10 validation";
        return false;
    }
    for (const auto& c : components->array) {
        if (!c.isObject() || c.object.size() != 2 || !publicIdentifier(c.get("id")) || !publicIdentifier(c.get("modelId"))) {
            error = "publication only supports component id/modelId; opaque fields require P10 validation";
            return false;
        }
    }
    return true;
}
}  // namespace

const MigrationChain& productionMigrations() {
    static const MigrationChain c{kSchemaVersion, {}};
    return c;
}

bool loadState(std::string_view text, StateDocument& out, std::string& error, const MigrationChain& chain, LoadReport* report) {
    JsonValue v;
    core::JsonLimits lim;
    lim.maxBytes = kSessionMaxBytes;
    if (!core::parseJson(text, v, error, lim)) return false;
    if (!v.isObject()) { error = "document not an object"; return false; }
    const JsonValue* k = v.get("kind");
    if (k && k->isString() && k->string == "scene" && text.size() > kSceneMaxBytes) { error = "scene exceeds 1.25 MiB"; return false; }
    const JsonValue* sv = v.get("schemaVersion");
    if (!isInt(sv, 0, 1e6)) { error = "schemaVersion must be a non-negative integer"; return false; }
    int ver = int(sv->number);
    if (ver > kSchemaVersion) { error = "incompatible newer schemaVersion"; return false; }
    const int start = ver;
    if (ver < chain.firstVersion || int(chain.steps.size()) != kSchemaVersion - chain.firstVersion) {
        error = "no migration path from schemaVersion " + std::to_string(ver);
        return false;
    }
    while (ver < kSchemaVersion) {
        MigrationFn fn = chain.steps[std::size_t(ver - chain.firstVersion)];
        std::string why;
        if (!(fn ? fn : noMigration)(v, why)) { error = "migration v" + std::to_string(ver) + " failed: " + why; return false; }
        const JsonValue* nv = v.isObject() ? v.get("schemaVersion") : nullptr;
        if (!isInt(nv, ver + 1, ver + 1)) { error = "migration v" + std::to_string(ver) + " did not advance version"; return false; }
        if (core::jsonDepth(v) > 20) { error = "migration exceeded depth bound"; return false; }
        ++ver;
    }
    StateDocument d;
    if (!validateV1(v, d, error)) return false;
    out = std::move(d);
    if (report) { report->sourceVersion = start; report->migrationsApplied = kSchemaVersion - start; }
    error.clear();
    return true;
}

std::string saveState(const StateDocument& doc) { return core::writeJson(toJson(doc, false)); }

bool publishScene(const StateDocument& doc, std::string& out, std::string& error) {
    if (doc.kind != DocKind::Scene) { error = "only scenes are published"; return false; }
    if (!publishableContent(doc, error)) return false;
    std::string candidate;
    try { candidate = core::writeJson(toJson(doc, true)); }
    catch (const std::invalid_argument& e) { error = e.what(); return false; }
    StateDocument check;
    if (!loadState(candidate, check, error)) return false;  // publishing never emits an unreadable scene
    out = std::move(candidate);
    error.clear();
    return true;
}

bool saveStateFileAtomic(const std::string& path, const std::string& text, std::string& error) {
    if (path.empty() || path.find('\0') != std::string::npos) { error = "invalid destination path"; return false; }
#if defined(_WIN32)
    if (path.size() > std::size_t(std::numeric_limits<int>::max())) { error = "destination path too long"; return false; }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), int(path.size()), nullptr, 0);
    if (!length) { error = "destination path must be UTF-8"; return false; }
    std::wstring destination(std::size_t(length), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), int(path.size()), destination.data(), length)) {
        error = "cannot convert destination path"; return false;
    }
    static std::atomic<unsigned long long> sequence{0};
    std::wstring temporary;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 128; ++attempt) {
        temporary = destination + L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"."
            + std::to_wstring(GetTickCount64()) + L"." + std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed));
        file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
        const auto cause = GetLastError();
        if (cause != ERROR_FILE_EXISTS && cause != ERROR_ALREADY_EXISTS) break;
    }
    if (file == INVALID_HANDLE_VALUE) { error = "cannot exclusively create temporary file"; return false; }
    bool written = true;
    for (std::size_t offset = 0; offset < text.size();) {
        const auto chunk = DWORD(std::min<std::size_t>(text.size() - offset, 1024 * 1024));
        DWORD count = 0;
        if (!WriteFile(file, text.data() + offset, chunk, &count, nullptr) || count == 0) { written = false; break; }
        offset += count;
    }
    const bool flushed = written && FlushFileBuffers(file);
    const bool closed = CloseHandle(file);
    if (!written || !flushed || !closed) {
        DeleteFileW(temporary.c_str()); error = "temporary write, flush or close failed"; return false;
    }
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str()); error = "replace failed"; return false;
    }
#else
    std::string pattern = path + ".tmp.XXXXXX";
    std::vector<char> temporary(pattern.begin(), pattern.end());
    temporary.push_back('\0');
    const int file = ::mkstemp(temporary.data());  // O_CREAT|O_EXCL, owner-only permissions
    if (file < 0) { error = "cannot exclusively create temporary file"; return false; }
    bool written = ::fcntl(file, F_SETFD, FD_CLOEXEC) != -1;
    for (std::size_t offset = 0; written && offset < text.size();) {
        const auto chunk = std::min<std::size_t>(text.size() - offset, 1024 * 1024);
        const auto count = ::write(file, text.data() + offset, chunk);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { written = false; break; }
        offset += std::size_t(count);
    }
    bool flushed = false;
    if (written) {
        int status;
        do { status = ::fsync(file); } while (status != 0 && errno == EINTR);
        flushed = status == 0;
    }
    const bool closed = ::close(file) == 0;  // Never retry close: descriptor reuse would be unsafe.
    if (!written || !flushed || !closed) {
        ::unlink(temporary.data()); error = "temporary write, flush or close failed"; return false;
    }
    if (::rename(temporary.data(), path.c_str()) != 0) {
        ::unlink(temporary.data()); error = "replace failed"; return false;
    }
#endif
    error.clear();
    return true;
}

RestoreDecision decideRestore(const StateDocument& doc, const RuntimeIdentity& rt, std::string& reason) {
    if (!doc.checkpoint) { reason = "no checkpoint; configuration only (no exact continuity claimed)"; return RestoreDecision::NoCheckpoint; }
    const EngineCheckpoint& c = *doc.checkpoint;
    if (c.engineModelVersion != rt.engineModelVersion) reason = "engine model version differs";
    else if (c.modelRevision != rt.modelRevision) reason = "model revision differs";
    else if (c.sampleRate != rt.sampleRate) reason = "sample rate differs";
    else if (c.oversampling != rt.oversampling) reason = "oversampling differs";
    else if (c.electrical.size() != rt.electricalSize || c.thermal.size() != rt.thermalSize) reason = "state layout differs";
    else { reason.clear(); return RestoreDecision::Restore; }
    reason += "; dynamic state reset to prepared warm initialisation";
    return RestoreDecision::ResetWarm;
}

}  // namespace cd::state
