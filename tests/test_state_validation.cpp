// CircuitDrift state: save-path validation regressions (validation repair packet, BRANCH_REVIEW
// 2026-10-30). Owner: cd_state tests. Threading: single-threaded test executable.
// Proves: ordinary save never emits text the loader rejects; in-memory (directly constructed)
// and parsed malformed values are rejected before any destination is replaced; reserved metadata
// collisions are rejected; valid caller data is preserved; createdAt calendar/clock validation
// with an explicit leap-second policy (SS=60 rejected); RNG layout in restore decisions.
// SYNTHETIC fixtures only; nothing here is a physical or factory preset.
#include "core/Json.h"
#include "state/StateDocument.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>
#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace {
using namespace cd;
using core::JsonValue;
int failures = 0;
#define CHECK(c) do { if (!(c)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

const char* kScene = R"({"schemaVersion":1,"product":"CircuitDrift","kind":"scene","engineModelVersion":"0.0.1-dev",
 "minReaderVersion":1,"author":"fixture","title":"Synthetic scene","tags":[],"createdAt":"2026-10-30T12:00:00Z",
 "requiredFeatures":["CD-F20"],"blueprint":{"schemaVersion":1,"id":"example.rc","components":[{"id":"R1","modelId":"fixture.resistor"}]},
 "baseParameters":{"hpf.frequency":20.5,"quality.oversampling":3},"macros":{},"timeline":{},"faults":[],"seed":42,
 "ui":{"theme":"dark"},"x-note":"kept"})";

state::StateDocument scene() {
    state::StateDocument d; std::string error;
    CHECK(state::loadState(kScene, d, error));
    return d;
}
state::StateDocument session() {
    auto d = scene();
    d.kind = state::DocKind::Session;
    d.checkpoint = state::EngineCheckpoint{"0.0.1-dev", "fixture-v1", 48000, 4, 100, {0.25, -0.5}, {300.0}, {1, 2}};
    return d;
}
std::string withCreatedAt(const std::string& value) {
    std::string text = kScene;
    text.replace(text.find("2026-10-30T12:00:00Z"), 20, value);
    return text;
}
std::string readFile(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}
JsonValue nested(int depth) {
    JsonValue v = JsonValue::makeNumber(0);
    for (int i = 0; i < depth; ++i) { JsonValue a = JsonValue::makeArray(); a.array.push_back(std::move(v)); v = std::move(a); }
    return v;
}

struct Case { const char* name; std::function<void(state::StateDocument&)> mutate; bool sessionBase; };

// Every case is a directly constructed in-memory document the loader would reject (or that
// serialization would silently alter). All three save entry points must refuse it.
std::vector<Case> invalidCases() {
    return {
        {"scene with checkpoint", [](auto& d) { d.checkpoint = state::EngineCheckpoint{"0.0.1-dev", "r", 48000, 1, 0, {}, {}, {}}; }, false},
        {"scene with no blueprint", [](auto& d) { d.blueprints.clear(); }, false},
        {"scene with two blueprints (second would be dropped)", [](auto& d) { d.blueprints.push_back(d.blueprints[0]); }, false},
        {"session with no blueprint", [](auto& d) { d.blueprints.clear(); }, true},
        {"session with three blueprints", [](auto& d) { d.blueprints.resize(3, d.blueprints[0]); }, true},
        {"blueprint not an object", [](auto& d) { d.blueprints[0] = JsonValue::makeArray(); }, false},
        {"blueprint unknown model", [](auto& d) { d.blueprints[0].get("components")->array[0].set("modelId", JsonValue::makeString("real.1N4148")); }, false},
        {"blueprint over 1 MiB", [](auto& d) { d.blueprints[0].set("notes", JsonValue::makeString(std::string(1048576, 'n'))); }, false},
        {"parameter out of range", [](auto& d) { d.baseParameters[0].second = 1e9; }, false},
        {"parameter NaN", [](auto& d) { d.baseParameters[0].second = std::nan(""); }, false},
        {"parameter unknown id", [](auto& d) { d.baseParameters.emplace_back("no.such.param", 0.0); }, false},
        {"discrete parameter non-integral", [](auto& d) { d.baseParameters[1].second = 2.5; }, false},
        {"duplicate parameter id (later would overwrite)", [](auto& d) { d.baseParameters.emplace_back(d.baseParameters[0].first, 30.0); }, false},
        {"engine model version", [](auto& d) { d.engineModelVersion = "9.9.9"; }, false},
        {"empty engine model version", [](auto& d) { d.engineModelVersion.clear(); }, false},
        {"minReaderVersion newer", [](auto& d) { d.minReaderVersion = 2; }, false},
        {"author too long", [](auto& d) { d.author.assign(257, 'a'); }, false},
        {"too many tags", [](auto& d) { d.tags.assign(33, "t"); }, false},
        {"empty tag", [](auto& d) { d.tags.assign(1, ""); }, false},
        {"unknown feature", [](auto& d) { d.requiredFeatures.push_back("CD-F99"); }, false},
        {"impossible createdAt", [](auto& d) { d.createdAt = "2026-02-30T25:61:61Z"; }, false},
        {"leap-second createdAt (policy: rejected)", [](auto& d) { d.createdAt = "2016-12-31T23:59:60Z"; }, false},
        {"empty createdAt", [](auto& d) { d.createdAt.clear(); }, false},
        {"seed above 2^53", [](auto& d) { d.seed = 9007199254740993ULL; }, false},
        {"ui not an object", [](auto& d) { d.ui = JsonValue::makeArray(); }, false},
        {"macros number (would be replaced by {})", [](auto& d) { d.macros = JsonValue::makeNumber(1); }, false},
        {"macros array", [](auto& d) { d.macros = JsonValue::makeArray(); }, false},
        {"timeline string", [](auto& d) { d.timeline = JsonValue::makeString("x"); }, false},
        {"faults bool", [](auto& d) { d.faults = JsonValue::makeBool(true); }, false},
        {"metadata reserved seed (would overwrite 42)", [](auto& d) { d.metadata.emplace_back("seed", JsonValue::makeNumber(99)); }, false},
        {"metadata reserved schemaVersion", [](auto& d) { d.metadata.emplace_back("schemaVersion", JsonValue::makeNumber(1)); }, false},
        {"metadata reserved product", [](auto& d) { d.metadata.emplace_back("product", JsonValue::makeString("CircuitDrift")); }, false},
        {"metadata reserved checkpoint", [](auto& d) { d.metadata.emplace_back("checkpoint", JsonValue::makeObject()); }, true},
        {"metadata reserved blueprints", [](auto& d) { d.metadata.emplace_back("blueprints", JsonValue::makeArray()); }, false},
        {"metadata duplicate key", [](auto& d) { d.metadata.emplace_back("x-note", JsonValue::makeString("again")); }, false},
        {"metadata over 64 KiB", [](auto& d) { d.metadata.emplace_back("x-big", JsonValue::makeString(std::string(65536, 'm'))); }, false},
        {"metadata depth over 20", [](auto& d) { d.metadata.emplace_back("x-deep", nested(25)); }, false},
        {"metadata non-finite number", [](auto& d) { d.metadata.emplace_back("x-inf", JsonValue::makeNumber(HUGE_VAL)); }, false},
        {"metadata malformed UTF-8 value", [](auto& d) { d.metadata.emplace_back("x-utf8", JsonValue::makeString(std::string("\xC0\xAF", 2))); }, false},
        {"metadata malformed UTF-8 key", [](auto& d) { d.metadata.emplace_back(std::string("x-\xED\xA0\x80", 5), JsonValue::makeNumber(1)); }, false},
        {"scene over 1.25 MiB", [](auto& d) {
            d.blueprints[0].set("notes", JsonValue::makeString(std::string(1000000, 'n')));
            d.ui->set("cache", JsonValue::makeString(std::string(400000, 'u'))); }, false},
        {"session over 2.5 MiB", [](auto& d) {
            d.blueprints[0].set("notes", JsonValue::makeString(std::string(1000000, 'n')));
            d.blueprints.push_back(d.blueprints[0]);
            d.ui->set("cache", JsonValue::makeString(std::string(700000, 'u'))); }, true},
        {"checkpoint oversampling 3", [](auto& d) { d.checkpoint->oversampling = 3; }, true},
        {"checkpoint sampleRate NaN", [](auto& d) { d.checkpoint->sampleRate = std::nan(""); }, true},
        {"checkpoint sampleRate low", [](auto& d) { d.checkpoint->sampleRate = 100; }, true},
        {"checkpoint sampleIndex above 2^53", [](auto& d) { d.checkpoint->sampleIndex = 9007199254740993ULL; }, true},
        {"checkpoint empty identity", [](auto& d) { d.checkpoint->modelRevision.clear(); }, true},
        {"checkpoint electrical NaN", [](auto& d) { d.checkpoint->electrical.push_back(std::nan("")); }, true},
        {"checkpoint electrical over 4096", [](auto& d) { d.checkpoint->electrical.assign(4097, 0.0); }, true},
        {"checkpoint rng over 64", [](auto& d) { d.checkpoint->rng.assign(65, 1); }, true},
    };
}

// T-STATE-SAVE: every invalid in-memory document is refused by both saveState forms and by
// saveStateFile, with a reason, while an existing destination stays byte-identical.
void saveRejectsInvalidInMemory(const std::filesystem::path& dir) {
    const auto dest = dir / "scene.circuitdrift.json";
    std::string error;
    CHECK(state::saveStateFile(dest.string(), scene(), error) && error.empty());
    const std::string original = readFile(dest);
    CHECK(!original.empty());
    int rejected = 0;
    for (const auto& c : invalidCases()) {
        auto d = c.sessionBase ? session() : scene();
        c.mutate(d);
        std::string out = "unchanged";
        error.clear();
        const bool ok = state::saveState(d, out, error);
        if (ok || out != "unchanged" || error.empty()) std::printf("  case not rejected: %s\n", c.name);
        CHECK(!ok && out == "unchanged" && !error.empty());
        bool threw = false;
        try { (void)state::saveState(d); } catch (const std::invalid_argument&) { threw = true; }
        CHECK(threw);
        error.clear();
        CHECK(!state::saveStateFile(dest.string(), d, error) && !error.empty());
        CHECK(readFile(dest) == original);
        rejected += !ok;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir))
        CHECK(entry.path().filename().string().find(".tmp.") == std::string::npos);
    std::printf("save validation: %d/%zu malformed in-memory documents rejected; destination intact\n",
                rejected, invalidCases().size());
}

// Parsed-input counterparts: the loader rejects the same malformed envelope values from text.
void parsedInputRejected() {
    state::StateDocument d; std::string error;
    const std::string sceneText = kScene;
    auto replaced = [&](const char* from, const char* to) {
        std::string t = sceneText;
        t.replace(t.find(from), std::string(from).size(), to);
        return t;
    };
    CHECK(!state::loadState(replaced("\"x-note\":\"kept\"", "\"x-note\":\"kept\",\"checkpoint\":{}"), d, error));
    CHECK(!state::loadState(replaced("\"hpf.frequency\":20.5", "\"hpf.frequency\":1e9"), d, error));
    CHECK(!state::loadState(replaced("\"macros\":{}", "\"macros\":[]"), d, error));
    CHECK(!state::loadState(replaced("\"x-note\":\"kept\"", "\"x-note\":\"kept\",\"seed\":99"), d, error));  // duplicate key
}

// T-STATE-PRESERVE: valid caller data, including nested/unicode metadata, two A/B blueprints,
// array timeline/faults and checkpoints, round-trips byte-identically; null payloads save as {}.
void validCallerDataPreserved(const std::filesystem::path& dir) {
    auto d = session();
    d.blueprints.push_back(d.blueprints[0]);
    d.blueprints[1].set("id", JsonValue::makeString("example.rc.b"));
    JsonValue meta = JsonValue::makeObject();
    meta.set("caf\xC3\xA9", JsonValue::makeString("\xF0\x9F\x98\x80"));
    meta.set("list", nested(5));
    d.metadata.emplace_back("x-vendor", meta);
    d.metadata.emplace_back("", JsonValue::makeBool(false));  // empty but unique, non-reserved key
    d.macros.set("age", JsonValue::makeNumber(0.5));
    d.timeline = JsonValue::makeArray(); d.timeline.array.push_back(JsonValue::makeNumber(1));
    std::string text, error;
    CHECK(state::saveState(d, text, error));
    state::StateDocument back;
    CHECK(state::loadState(text, back, error));
    CHECK(back.blueprints.size() == 2 && back.metadata.size() == d.metadata.size());
    CHECK(back.seed == 42 && back.macros.get("age") && back.timeline.isArray());
    CHECK(back.checkpoint && back.checkpoint->rng == d.checkpoint->rng && back.checkpoint->electrical == d.checkpoint->electrical);
    for (std::size_t i = 0; i < d.metadata.size(); ++i)
        CHECK(back.metadata[i].first == d.metadata[i].first && core::writeJson(back.metadata[i].second) == core::writeJson(d.metadata[i].second));
    CHECK(state::saveState(back) == text);
    const auto dest = dir / "session.circuitdrift.json";
#if !defined(_WIN32)
    const mode_t mask = ::umask(022); ::umask(mask);  // sample the process umask before saving
    { std::ofstream existing(dest); existing << "old"; }
    ::chmod(dest.c_str(), 0640);
#endif
    CHECK(state::saveStateFile(dest.string(), d, error) && readFile(dest) == text);
    auto nulls = scene();
    nulls.macros = JsonValue(); nulls.timeline = JsonValue(); nulls.faults = JsonValue();
    CHECK(state::saveState(nulls, text, error));
    CHECK(state::loadState(text, back, error) && back.macros.isObject() && back.macros.object.empty());
#if !defined(_WIN32)
    // Atomic-save permission behaviour unchanged: owner-only (0600) replacement inode even over a
    // 0640 file (documented private-file policy), and the save never mutates the process umask.
    struct stat st{};
    CHECK(::stat(dest.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    const mode_t after = ::umask(mask); ::umask(after);
    CHECK(after == mask);
#endif
}

// T-STATE-TIME: createdAt calendar/clock ranges. Leap seconds (SS=60) rejected by policy.
void createdAtCalendar() {
    state::StateDocument d; std::string error;
    for (const char* bad : {"2026-02-29T00:00:00Z", "2100-02-29T00:00:00Z", "1900-02-29T12:00:00Z", "2026-04-31T00:00:00Z",
                            "2026-06-31T00:00:00Z", "2026-09-31T00:00:00Z", "2026-11-31T00:00:00Z", "2026-13-01T00:00:00Z",
                            "2026-00-10T00:00:00Z", "2026-01-00T00:00:00Z", "2026-01-32T00:00:00Z", "2026-10-30T24:00:00Z",
                            "2026-10-30T23:60:00Z", "2026-10-30T23:59:60Z", "2016-12-31T23:59:60Z", "2026-02-30T25:61:61Z",
                            "2026-10-30T12:00:00.Z", "2026-10-30t12:00:00Z", "2026-10-30T12:00:00z", "2026-10-30T12:00:00+00:00"}) {
        if (state::loadState(withCreatedAt(bad), d, error)) std::printf("  accepted createdAt %s\n", bad);
        CHECK(!state::loadState(withCreatedAt(bad), d, error));
        auto mem = scene(); mem.createdAt = bad;
        std::string out;
        CHECK(!state::saveState(mem, out, error));
    }
    for (const char* good : {"2024-02-29T00:00:00Z", "2000-02-29T23:59:59Z", "0000-02-29T00:00:00Z", "2026-12-31T23:59:59Z",
                             "2026-01-31T00:00:00Z", "2026-04-30T00:00:00Z", "9999-12-31T23:59:59.999999Z", "2026-10-30T12:00:00.5Z"}) {
        if (!state::loadState(withCreatedAt(good), d, error)) std::printf("  rejected createdAt %s: %s\n", good, error.c_str());
        CHECK(state::loadState(withCreatedAt(good), d, error) && d.createdAt == good);
        std::string out;
        CHECK(state::saveState(d, out, error));
    }
}

// T-STATE-RNG: restore requires the declared runtime RNG state count.
void rngLayoutRestore() {
    std::string reason;
    state::RuntimeIdentity rt{"0.0.1-dev", "fixture-v1", 48000, 4, 2, 1, std::size_t(2)};
    int restores = 0;
    for (std::size_t n : {0u, 1u, 2u, 3u, 64u}) {
        auto d = session(); d.checkpoint->rng.assign(n, 7);
        const auto decision = state::decideRestore(d, rt, reason);
        restores += decision == state::RestoreDecision::Restore;
        CHECK((n == 2) == (decision == state::RestoreDecision::Restore));
        if (n != 2) CHECK(decision == state::RestoreDecision::ResetWarm && reason.find("RNG layout differs") != std::string::npos);
    }
    CHECK(restores == 1);
    auto undeclared = rt; undeclared.rngStateCount.reset();
    CHECK(state::decideRestore(session(), undeclared, reason) == state::RestoreDecision::ResetWarm
          && reason.find("not declared") != std::string::npos);
    auto noRng = session(); noRng.checkpoint->rng.clear();
    auto zero = rt; zero.rngStateCount = 0;
    CHECK(state::decideRestore(noRng, zero, reason) == state::RestoreDecision::Restore && reason.empty());
    CHECK(state::decideRestore(scene(), rt, reason) == state::RestoreDecision::NoCheckpoint);
}

// T-STATE-META-LINEAR: bounded maximum-count metadata saves and reloads intact (informational time).
void boundedMetadataCount() {
    auto d = scene();
    d.metadata.clear();
    static const char a[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (int i = 0; i < 16000; ++i) {  // 3-byte key + "0" = 4 bytes each: 64000 B <= 64 KiB bound
        std::string k = {a[i % 62], a[(i / 62) % 62], a[(i / 3844) % 62]};
        d.metadata.emplace_back(k, JsonValue::makeNumber(0));
    }
    std::string text, error;
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(state::saveState(d, text, error));
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    state::StateDocument back;
    CHECK(state::loadState(text, back, error) && back.metadata.size() == 16000);
    std::printf("16000-entry metadata validated save: %.1f ms (sanitizer build; informational, not a CPU qualification)\n", ms);
}
}  // namespace

int main() {
    namespace fs = std::filesystem;
    fs::path dir;
    for (unsigned n = 0; n < 10000 && dir.empty(); ++n) {
        auto candidate = fs::temp_directory_path() / ("circuitdrift-state-validation-" + std::to_string(n));
        if (fs::create_directory(candidate)) dir = candidate;
    }
    CHECK(!dir.empty());
    if (!dir.empty()) { saveRejectsInvalidInMemory(dir); validCallerDataPreserved(dir); fs::remove_all(dir); }
    parsedInputRejected(); createdAtCalendar(); rngLayoutRestore(); boundedMetadataCount();
    std::printf("state validation regressions: %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
