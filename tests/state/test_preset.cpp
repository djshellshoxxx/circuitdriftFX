// Scene presets, factory presets, preset library and strict-reader hostile-input tests
// (CD-F09/F24; T24 config roundtrip). Synthetic data only.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <stdexcept>
#include <vector>
#include "state/Preset.h"
#include "state/PresetLibrary.h"
#include "state/PresetCatalogWorker.h"
#include "check.h"

using namespace cd;
using namespace cd::state;
namespace fs = std::filesystem;

namespace {
bool replaceOnce(std::string& s, const std::string& from, const std::string& to) {
    const auto p = s.find(from);
    if (p == std::string::npos) return false;
    s.replace(p, from.size(), to);
    return true;
}

Config randomConfig(std::mt19937_64& rng) {
    Config c;
    for (std::size_t i = 0; i < core::kParamCount; ++i) {
        const core::ParamSpec& s = core::kParams[i];
        if (s.kind == core::ParamKind::Float) {
            std::uniform_real_distribution<double> d(s.min, s.max);
            c.set(i, d(rng));
        } else {
            std::uniform_int_distribution<long long> d(static_cast<long long>(s.min), static_cast<long long>(s.max));
            c.set(i, double(d(rng)));
        }
    }
    for (auto& sl : c.slots) { sl.blueprintId = "bp." + std::to_string(rng() % 1000); sl.seed = rng() % (kMaxSeed + 1); }
    return c;
}

std::string cleanText() {
    ScenePreset p = factoryPresets().front().scene;
    std::string text, err;
    CHECK_MSG(sceneToJson(p, text, err), err);
    return text;
}

void expectReject(const std::string& text, const char* what) {
    ScenePreset p;
    std::string err;
    const bool ok = sceneFromJson(text, p, err);
    CHECK_MSG(!ok, std::string("accepted hostile input: ") + what);
    CHECK_MSG(ok || !err.empty(), std::string("no error message: ") + what);
}

void testFactory() {
    const auto& f = factoryPresets();
    CHECK(f.size() >= 12);
    std::set<std::string> ids, titles;
    for (std::size_t i = 0; i < f.size(); ++i) {
        ids.insert(f[i].id);
        titles.insert(f[i].scene.meta.title);
        for (std::size_t j = i + 1; j < f.size(); ++j) CHECK_MSG(!(f[i].scene.slot == f[j].scene.slot && f[i].scene.globals == f[j].scene.globals), f[i].id + " duplicates " + f[j].id);
        // Every registry value valid (never clamped) and the scene survives the strict reader.
        std::string err;
        for (std::size_t p = 0; p < kSlotParamCount; ++p) CHECK_MSG(validateParamValue(kSlotAIndices[p], f[i].scene.slot.params[p], err), f[i].id + ": " + err);
        for (std::size_t g = 0; g < kSceneGlobalCount; ++g) CHECK_MSG(validateParamValue(kSceneGlobalIndices[g], f[i].scene.globals[g], err), f[i].id + ": " + err);
        std::string text;
        CHECK_MSG(sceneToJson(f[i].scene, text, err), f[i].id + ": " + err);
        ScenePreset back;
        CHECK_MSG(sceneFromJson(text, back, err), f[i].id + ": " + err);
        CHECK(back == f[i].scene);
        CHECK(f[i].scene.meta.author == "CircuitDrift Factory");
        // 100% wet and Auto-Gain on for every factory scene (UX-02).
        CHECK(f[i].scene.globals[5] == 1.0 && f[i].scene.globals[6] == 1.0);
    }
    CHECK(ids.size() == f.size() && titles.size() == f.size());
    for (const char* t : {"Clean Console", "Mint 1968 Console", "Unserviced Studio 1978", "Brownout Tube Preamp", "Selenium Sag Pump",
                          "Warm Iron Transformer", "JFET Glue", "BJT Grit", "Thermal Bloom", "Gigged Road Amp", "Hum & Hiss Lo-Fi",
                          "Sidechain Pump Ready", "Warm Tube Preamp", "Pumping Bus Glue", "Lo-Fi Attic Gear"})
        CHECK_MSG(titles.count(t) == 1, std::string("missing factory preset ") + t);

    // Clean Console: macros, noise, leakage off; calibrated clean base (spec 02, UX-02).
    const ScenePreset& cc = f.front().scene;
    CHECK(cc.meta.title == "Clean Console");
    for (int i = 0; i < 5; ++i) CHECK(cc.globals[std::size_t(i)] == 0.0);  // 4 macros + analogNoise.enabled
    CHECK(cc.slot.params[6] == 0.0);  // capLeakage
    CHECK(cc.slot.params[16] == -120.0 && cc.slot.params[17] == -120.0 && cc.slot.params[18] == 0.0);  // hum, thermal, microphonics
    CHECK(std::string(core::kParams[kSlotAIndices[6]].id) == "slot.a.condition.capLeakage");
    CHECK(std::string(core::kParams[kSlotAIndices[16]].id) == "slot.a.noise.humLevel");
    CHECK(std::string(core::kParams[kSlotAIndices[17]].id) == "slot.a.noise.thermalNoise");
    CHECK(std::string(core::kParams[kSlotAIndices[18]].id) == "slot.a.noise.microphonics");
    // UX-02 quick preset macro table.
    auto byId = [&](const char* id) -> const ScenePreset& { for (const auto& x : f) if (x.id == id) return x.scene; return f.front().scene; };
    CHECK(byId("warm-tube-preamp").globals[0] == 0.15 && byId("warm-tube-preamp").globals[3] == 0.55);
    CHECK(byId("lo-fi-attic-gear").globals[0] == 0.85 && byId("lo-fi-attic-gear").globals[4] == 1.0);
}

void testRoundtrip() {
    // save -> load -> save is a byte-exact fixed point for every factory scene.
    for (const auto& f : factoryPresets()) {
        std::string t1, t2, err;
        CHECK(sceneToJson(f.scene, t1, err));
        ScenePreset p;
        CHECK(sceneFromJson(t1, p, err));
        CHECK(sceneToJson(p, t2, err));
        CHECK_MSG(t1 == t2, f.id);
    }
    // Whole-config JSON roundtrip is bit exact for random doubles (T24).
    std::mt19937_64 rng(99);
    for (int n = 0; n < 200; ++n) {
        Config c = randomConfig(rng);
        if (n == 0) { c.set(core::indexOf("input.gain"), 0.1 + 0.2); c.set(core::indexOf("mix"), std::nextafter(1.0, 0.0)); }
        if (n == 1) { c.set(core::indexOf("input.gain"), -0.0); c.set(core::indexOf("mix"), 5e-324); }
        std::string t1, t2, err;
        PresetMeta m; m.title = "t"; m.author = "a"; m.tags = {"x"};
        CHECK_MSG(configToSessionJson(c, m, t1, err), err);
        Config back;
        CHECK_MSG(configFromSessionJson(t1, back, err), err);
        CHECK(back == c);
        for (std::size_t i = 0; i < core::kParamCount; ++i) {
            const double a = back.get(i), b = c.get(i);
            if (a != b || (a == 0.0 && std::signbit(a) != std::signbit(b) && n != 1)) { CHECK_MSG(false, std::string("not bit exact: ") + core::kParams[i].id); break; }
        }
        CHECK(configToSessionJson(back, m, t2, err) && t1 == t2);
    }
    // Scene roundtrip with random slot values.
    for (int n = 0; n < 50; ++n) {
        const Config c = randomConfig(rng);
        const ScenePreset p = captureScene(c, n & 1, PresetMeta{"Random", "me", {"t1", "t2"}, "2026-02-03T04:05:06Z"});
        std::string t, err;
        CHECK_MSG(sceneToJson(p, t, err), err);
        ScenePreset q;
        CHECK_MSG(sceneFromJson(t, q, err), err);
        CHECK(q == p);
    }
}

void testModified() {
    std::mt19937_64 rng(5);
    Config cur = randomConfig(rng);
    const ScenePreset& clean = factoryPresets().front().scene;
    cur = applyScene(cur, 0, clean);
    CHECK(matchesScene(cur, 0, clean) && !isModified(cur, 0, clean));
    // Host/UI-level params (gain, HPF, quality, crossfade) never dirty a scene.
    cur.set(core::indexOf("output.gain"), 5.0);
    cur.set(core::indexOf("quality.mode"), 2.0);
    cur.set(core::indexOf("ab.crossfade"), 0.4);
    CHECK(!isModified(cur, 0, clean));
    // Slot B edits do not modify a scene loaded into A; slot A edits do.
    cur.set(core::indexOf("slot.b.stage.drive"), 30.0);
    CHECK(!isModified(cur, 0, clean));
    Config edited = cur;
    edited.set(core::indexOf("slot.a.stage.drive"), 7.0);
    CHECK(isModified(edited, 0, clean));
    edited.set(core::indexOf("slot.a.stage.drive"), clean.slot.params[11]);  // back to the preset value
    CHECK(!isModified(edited, 0, clean));
    // Macro edit: modified, except for circuit-only loads.
    edited.set(core::indexOf("macro.warmth"), 0.2);
    CHECK(isModified(edited, 0, clean) && !isModified(edited, 0, clean, true));
    edited.slots[0].seed += 1;
    CHECK(isModified(edited, 0, clean, true));
    // Loading to B / circuit-only preserves macros.
    Config base = randomConfig(rng);
    const Config loadedB = applyScene(base, 1, factoryPresets()[3].scene, true);
    CHECK(loadedB.slots[0] == base.slots[0] && loadedB.get(core::indexOf("macro.sag")) == base.get(core::indexOf("macro.sag")));
    CHECK(loadedB.slots[1] == factoryPresets()[3].scene.slot);

    // These public scene helpers are called with a UI/service slot index.
    // Negative and out-of-range indices must fail before accessing the slots array.
    const auto slotRejected = [](auto&& operation) {
        try { operation(); }
        catch (const std::out_of_range&) { return true; }
        catch (...) { return false; }
        return false;
    };
    const Config unchanged = base;
    for (const int invalid : {-1, 2, std::numeric_limits<int>::max()}) {
        CHECK(slotRejected([&] { (void)captureScene(base, invalid, PresetMeta{}); }));
        CHECK(slotRejected([&] { (void)applyScene(base, invalid, clean); }));
        CHECK(slotRejected([&] { (void)matchesScene(base, invalid, clean); }));
        CHECK(base == unchanged);
    }
}

void testHostile() {
    const std::string good = cleanText();
    ScenePreset p;
    std::string err;
    CHECK_MSG(sceneFromJson(good, p, err), err);

    std::string t;
    t = good; CHECK(replaceOnce(t, "\"mix\":1", "\"mix\":1,\"mix\":1")); expectReject(t, "duplicate key");
    t = good; CHECK(replaceOnce(t, "\"mix\":1", "\"mix\":1.5")); expectReject(t, "out of range (not clamped)");
    t = good; CHECK(replaceOnce(t, "\"mix\":1", "\"mix\":-0.0001")); expectReject(t, "below range");
    t = good; CHECK(replaceOnce(t, "\"mix\":1", "\"mix\":NaN")); expectReject(t, "NaN literal");
    t = good; CHECK(replaceOnce(t, "\"mix\":1", "\"mix\":Infinity")); expectReject(t, "Infinity literal");
    t = good; CHECK(replaceOnce(t, "\"mix\":1", "\"mix\":1e999")); expectReject(t, "overflow");
    t = good; CHECK(replaceOnce(t, "\"mix\":1", "\"mix\":\"1\"")); expectReject(t, "string for number");
    t = good; CHECK(replaceOnce(t, "\"slot.a.stage.activeElement\":2", "\"slot.a.stage.activeElement\":1.5")); expectReject(t, "non-integral enum");
    t = good; CHECK(replaceOnce(t, "\"mix\":1", "\"mix\":1,\"slot.b.stage.drive\":3")); expectReject(t, "slot B key in scene");
    t = good; CHECK(replaceOnce(t, "\"mix\":1", "\"mix\":1,\"input.gain\":3")); expectReject(t, "host-level key in scene");
    t = good; CHECK(replaceOnce(t, "\"mix\":1", "\"mix\":1,\"no.such.param\":3")); expectReject(t, "unknown parameter");
    t = good; CHECK(replaceOnce(t, "\"product\"", "\"x-unknown\":1,\"product\"")); expectReject(t, "unknown top-level field");
    t = good; CHECK(replaceOnce(t, "\"product\"", "\"ui\":{},\"product\"")); expectReject(t, "ui present");
    t = good; CHECK(replaceOnce(t, "\"macros\":{}", "\"macros\":{\"a\":1}")); expectReject(t, "non-empty macros");
    t = good; CHECK(replaceOnce(t, "\"components\":[]", "\"components\":[{\"id\":\"R1\",\"modelId\":\"fixture.resistor\"}]")); expectReject(t, "blueprint components");
    t = good; CHECK(replaceOnce(t, "\"components\":[]", "\"components\":[],\"extra\":1")); expectReject(t, "blueprint extra key");
    t = good; CHECK(replaceOnce(t, "\"kind\":\"scene\"", "\"kind\":\"session\"")); expectReject(t, "kind mismatch");
    t = good; CHECK(replaceOnce(t, "\"engineModelVersion\":\"0.0.1-dev\"", "\"engineModelVersion\":\"9.9\"")); expectReject(t, "unknown engine version");
    t = good; CHECK(replaceOnce(t, "\"requiredFeatures\":[", "\"requiredFeatures\":[\"CD-F99\",")); expectReject(t, "unknown required feature");
    t = good; CHECK(replaceOnce(t, "\"seed\":1001", "\"seed\":-1")); expectReject(t, "negative seed");
    t = good; CHECK(replaceOnce(t, "\"seed\":1001", "\"seed\":9007199254740993")); expectReject(t, "seed above 2^53");
    t = good; CHECK(replaceOnce(t, "\"seed\":1001", "\"seed\":1.5")); expectReject(t, "fractional seed");
    t = good; CHECK(replaceOnce(t, "\"schemaVersion\":1", "\"schemaVersion\":2")); expectReject(t, "newer schema");
    expectReject(t = good.substr(0, good.size() / 2), "truncated");
    expectReject(good + "x", "trailing data");
    expectReject("", "empty");
    expectReject("[]", "array root");
    expectReject("null", "null root");
    expectReject(std::string("\xEF\xBB\xBF") + good, "BOM");
    t = good; CHECK(replaceOnce(t, "\"CircuitDrift Factory\"", "\"bad\xC3\x28\"")); expectReject(t, "invalid UTF-8");
    // Huge: > 1.25 MiB scene bound.
    t = good; CHECK(replaceOnce(t, "\"title\":\"", "\"title\":\"" + std::string(1400000, 'x'))); expectReject(t, "oversize scene");
    // Deep nesting beyond depth 20.
    std::string deep;
    for (int i = 0; i < 40; ++i) deep += "{\"a\":";
    deep += "1";
    for (int i = 0; i < 40; ++i) deep += "}";
    t = good; CHECK(replaceOnce(t, "\"macros\":{}", "\"macros\":" + deep)); expectReject(t, "deep nesting");
    // Writer refuses to emit invalid content instead of clamping it.
    ScenePreset bad = factoryPresets().front().scene;
    std::string out;
    bad.globals[5] = 2.0;
    CHECK(!sceneToJson(bad, out, err) && out.empty());
    bad = factoryPresets().front().scene; bad.slot.params[0] = std::nan("");
    CHECK(!sceneToJson(bad, out, err));
    bad = factoryPresets().front().scene; bad.slot.blueprintId = "";
    CHECK(!sceneToJson(bad, out, err));
    bad = factoryPresets().front().scene; bad.slot.seed = kMaxSeed + 1;
    CHECK(!sceneToJson(bad, out, err));
    // Missing parameters default to the registry (partial documents are valid, never "unset").
    t = good;
    ScenePreset partial;
    std::string minimal = "{\"schemaVersion\":1,\"product\":\"CircuitDrift\",\"kind\":\"scene\",\"engineModelVersion\":\"0.0.1-dev\","
        "\"minReaderVersion\":1,\"author\":\"\",\"title\":\"p\",\"tags\":[],\"createdAt\":\"2026-01-01T00:00:00Z\",\"requiredFeatures\":[],"
        "\"blueprint\":{\"schemaVersion\":1,\"id\":\"b\",\"components\":[]},\"baseParameters\":{\"slot.a.stage.drive\":9},"
        "\"macros\":{},\"timeline\":{},\"faults\":[],\"seed\":0}";
    CHECK_MSG(sceneFromJson(minimal, partial, err), err);
    CHECK(partial.slot.params[11] == 9.0 && partial.globals[5] == 1.0 && partial.slot.params[0] == 117.0);
}

void testSanitize() {
    std::string e;
    for (const char* ok : {"My Preset", "a-b_c (2)", "v1.2", "Clean Console", "x"}) CHECK_MSG(sanitizeStem(ok, e), std::string(ok) + ": " + e);
    const std::vector<std::string> bad = {
        "", "..", "../x", "a/b", "a\\b", "..\\x", "..\\..\\Windows", "/etc/passwd", "C:", "C:\\x", "a:b", "CON", "con", "Con.txt", "PRN", "aux",
        "NUL", "nul.json", "COM1", "com9", "LPT1", "lpt9.cfg", "CON .txt", ".hidden", "trail.", "trail ", " lead", "x..y", "a\tb",
        "a*b", "a?b", "a<b", "a>b", "a|b", "a\"b", "caf\xC3\xA9", std::string("a\0b", 3), std::string(kMaxStemBytes + 1, 'a')};
    for (const auto& b : bad) CHECK_MSG(!sanitizeStem(b, e), "accepted unsafe name: " + b);
    CHECK(sanitizeStem(std::string(kMaxStemBytes, 'a'), e));
    CHECK(sanitizeStem("COM10", e));  // not a reserved device name
}

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() / ("cd_state_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};

std::size_t countEntries(const fs::path& d) { std::size_t n = 0; for (auto it = fs::directory_iterator(d); it != fs::directory_iterator(); ++it) ++n; return n; }

void testLibrary() {
    TempDir td;
    const fs::path userDir = td.path / "presets";  // does not exist yet: save creates it
    PresetLibrary lib(userDir);
    std::string err, id;

    auto all = lib.list();
    CHECK(all.size() == factoryPresets().size());  // missing user dir is fine
    CHECK(all.front().id == "factory:clean-console" && all.front().factory);

    ScenePreset p = factoryPresets()[5].scene;
    p.meta.title = "My Studio Sound"; p.meta.author = "tester"; p.meta.tags = {"mine", "studio"};
    CHECK_MSG(lib.save("My Studio Sound", p, false, id, err), err);
    CHECK(id == "user:My Studio Sound");
    CHECK(!lib.save("My Studio Sound", p, false, id, err));  // exists
    CHECK(lib.save("My Studio Sound", p, true, id, err));    // overwrite allowed
    CHECK(countEntries(userDir) == 1);                       // atomic save leaves no temp files
    ScenePreset back;
    CHECK_MSG(lib.load(id, back, err), err);
    CHECK(back == p);

    // Unsafe names are rejected and nothing is written anywhere.
    const std::size_t before = countEntries(td.path);
    for (const char* n : {"..\\evil", "../evil", "a/b", "CON", "..", ""}) CHECK(!lib.save(n, p, true, id, err));
    CHECK(countEntries(td.path) == before && countEntries(userDir) == 1);
    CHECK(!lib.load("user:../evil", back, err));
    CHECK(!lib.load("user:CON", back, err));
    CHECK(!lib.load("nonsense", back, err));
    CHECK(!lib.load("factory:nope", back, err));
    CHECK(!lib.remove("factory:clean-console", err));
    CHECK(!lib.remove("user:../x", err));

    // Search: case-insensitive over id/title/author/tags.
    CHECK(lib.list("STUDIO").size() >= 2);  // "Unserviced Studio 1978" + user preset
    CHECK(lib.list("mine").size() == 1);
    CHECK(lib.list("zzz-no-match").empty());
    CHECK(lib.list("tester").size() == 1 && lib.list("tester")[0].id == "user:My Studio Sound");

    // Hostile and corrupt files in the directory are listed as invalid, never loaded.
    {
        std::ofstream(userDir / "Broken.circuitdrift.json", std::ios::binary) << "{ not json";
        std::ofstream(userDir / "notes.txt", std::ios::binary) << "ignored";
        std::string hostile = cleanText();
        replaceOnce(hostile, "\"mix\":1", "\"mix\":9");
        std::ofstream(userDir / "Hostile.circuitdrift.json", std::ios::binary) << hostile;
    }
    std::size_t invalid = 0;
    for (const auto& i : lib.list()) if (!i.valid) { ++invalid; CHECK(!i.error.empty()); }
    CHECK(invalid == 2);
    CHECK(!lib.load("user:Broken", back, err));
    CHECK(!lib.load("user:Hostile", back, err));

    // Export / import round trip; hostile import writes nothing.
    const fs::path exported = td.path / "export.circuitdrift.json";
    CHECK_MSG(lib.exportPreset(id, exported, err), err);
    CHECK_MSG(lib.exportPreset("factory:jfet-glue", td.path / "jfet.circuitdrift.json", err), err);
    PresetLibrary lib2(td.path / "other");
    std::string id2;
    CHECK_MSG(lib2.importFile(exported, "", false, id2, err), err);
    CHECK(id2 == "user:export");
    ScenePreset imported;
    CHECK(lib2.load(id2, imported, err) && imported == p);
    CHECK(lib2.importFile(td.path / "presets" / "Hostile.circuitdrift.json", "", false, id2, err) == false);
    CHECK(lib2.importFile(td.path / "presets" / "Broken.circuitdrift.json", "x", false, id2, err) == false);
    CHECK(lib2.importFile(td.path / "missing.json", "x", false, id2, err) == false);
    CHECK(!lib2.importFile(exported, "../escape", true, id2, err));
    CHECK(countEntries(td.path / "other") == 1);
    {   // oversize file rejected by size check before parsing
        std::ofstream f(td.path / "big.circuitdrift.json", std::ios::binary);
        f << std::string(kSceneMaxBytes + 1, ' ');
    }
    CHECK(!lib2.importFile(td.path / "big.circuitdrift.json", "big", false, id2, err));

    // Both existing-target preservation and a real two-writer race must be
    // enforced at installation time (the old exists()+rename path lost the race).
    {
        const fs::path exclusive = td.path / "exclusive-test.txt";
        CHECK_MSG(saveStateFileAtomicNoReplace(exclusive.string(), "first", err), err);
        CHECK(!saveStateFileAtomicNoReplace(exclusive.string(), "second", err));
        std::ifstream in(exclusive, std::ios::binary);
        std::string contents;
        std::getline(in, contents);
        CHECK(contents == "first");

        const fs::path concurrent = td.path / "concurrent-test.txt";
        std::atomic<int> ready{0};
        std::atomic<bool> start{false};
        bool succeeded[2] = {false, false};
        const auto attempt = [&](int index) {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            std::string localError;
            succeeded[index] = saveStateFileAtomicNoReplace(
                concurrent.string(), index == 0 ? "alpha" : "beta", localError);
        };
        std::thread first(attempt, 0), second(attempt, 1);
        while (ready.load(std::memory_order_acquire) != 2) std::this_thread::yield();
        start.store(true, std::memory_order_release);
        first.join();
        second.join();
        CHECK(succeeded[0] != succeeded[1]);
        std::ifstream installed(concurrent, std::ios::binary);
        std::string winner;
        std::getline(installed, winner);
        CHECK(winner == (succeeded[0] ? "alpha" : "beta"));
        for (const auto& entry : fs::directory_iterator(td.path)) {
            const auto filename = entry.path().filename().string();
            CHECK(filename.find("exclusive-test.txt.tmp.") != 0);
            CHECK(filename.find("concurrent-test.txt.tmp.") != 0);
        }
    }

    // The byte limit is inclusive. Padding with legal JSON whitespace does
    // not change the scene, and one extra byte must reject the whole import.
    std::string boundary = cleanText();
    boundary.resize(kSceneMaxBytes, ' ');
    const fs::path boundaryPath = td.path / "boundary.circuitdrift.json";
    { std::ofstream f(boundaryPath, std::ios::binary); f.write(boundary.data(), std::streamsize(boundary.size())); }
    CHECK_MSG(lib2.importFile(boundaryPath, "Boundary", false, id2, err), err);
    ScenePreset boundaryScene;
    CHECK(lib2.load(id2, boundaryScene, err) && boundaryScene == factoryPresets().front().scene);
    const std::string previousId = id2;
    const std::size_t previousEntries = countEntries(td.path / "other");
    { std::ofstream f(boundaryPath, std::ios::binary | std::ios::app); f.put(' '); }
    CHECK(!lib2.importFile(boundaryPath, "Too large", false, id2, err));
    CHECK(id2 == previousId && countEntries(td.path / "other") == previousEntries);

    CHECK(lib.remove(id, err));
    CHECK(!lib.remove(id, err));
    CHECK(!lib.load(id, back, err));
}

void testCatalogBudgets() {
    TempDir td;
    const auto notes = td.path / "notes";
    fs::create_directories(notes);
    for (int i = 0; i < 4; ++i) std::ofstream(notes / (std::to_string(i) + ".txt")) << "ordinary note";
    const auto noMatches = PresetLibrary(notes).scan({2, kMaxCatalogReadBytes});
    CHECK(noMatches.visited == 2 && noMatches.truncated && noMatches.bytesRead == 0);
    CHECK(filterPresets(noMatches, "absent search term").empty());
    CHECK(noMatches.entries.size() == factoryPresets().size());

    const auto scenes = td.path / "scenes";
    fs::create_directories(scenes);
    const std::string text = cleanText();
    for (int i = 0; i < 3; ++i)
        std::ofstream(scenes / (std::to_string(i) + std::string(kPresetExtension)), std::ios::binary) << text;
    PresetLibrary library(scenes);
    const auto byBytes = library.scan({kMaxListedPresets, text.size()});
    CHECK(byBytes.truncated && byBytes.bytesRead == text.size());
    CHECK(byBytes.entries.size() == factoryPresets().size() + 1 && byBytes.entries.back().valid);
    const auto byEntries = library.scan({1, kMaxCatalogReadBytes});
    CHECK(byEntries.truncated && byEntries.visited == 1 && byEntries.bytesRead == text.size());
    const auto all = library.scan({3, text.size() * 3});
    CHECK(!all.truncated && all.visited == 3 && all.bytesRead == text.size() * 3);
    CHECK(filterPresets(all, "user:").size() == 3);
    fs::remove_all(scenes);
    CHECK(filterPresets(all, "user:").size() == 3); // search remains a pure snapshot operation

    CHECK(PresetLibrary(scenes).scan().error.empty()); // first-use missing folder is normal
    const auto ordinaryFile = td.path / "not-a-directory";
    std::ofstream(ordinaryFile) << "ordinary file";
    const auto failed = PresetLibrary(ordinaryFile).scan();
    CHECK(!failed.error.empty() && !failed.truncated && failed.visited == 0);
    CHECK(failed.entries.size() == factoryPresets().size());
    std::stop_source cancel;
    cancel.request_stop();
    const auto stopped = PresetLibrary(ordinaryFile).scan({}, cancel.get_token());
    CHECK(stopped.cancelled && stopped.error.empty() && stopped.entries.empty());
    CHECK(stopped.visited == 0 && stopped.bytesRead == 0);
}

bool waitForCatalog(PresetCatalogWorker& worker, PresetCatalogResult& result) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (worker.poll(result)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

void testCatalogWorker() {
    // Hold the first provider call in flight, then queue many refreshes. This
    // deterministically exercises cancellation, coalescing and stale-result
    // rejection without relying on directory traversal speed or sleep timing.
    std::mutex gate;
    std::condition_variable ready;
    bool entered = false, release = false;
    std::atomic<int> calls{0};
    std::atomic<bool> scannedOnCaller{false}, cancelled{false};
    const auto caller = std::this_thread::get_id();
    PresetCatalogWorker worker([&](std::stop_token stop) {
        if (std::this_thread::get_id() == caller) scannedOnCaller.store(true);
        const int call = ++calls;
        if (call == 1) {
            std::unique_lock lock(gate);
            entered = true;
            ready.notify_one();
            ready.wait_for(lock, std::chrono::seconds(10), [&] { return release; });
            cancelled.store(stop.stop_requested());
        }
        PresetCatalog catalog;
        PresetInfo row;
        row.id = call == 1 ? "user:stale" : "user:latest";
        catalog.entries.push_back(std::move(row));
        return catalog; // intentionally returns data even if this request was cancelled
    });
    CHECK(calls.load() == 0); // lazy construction
    const auto first = worker.request();
    {
        std::unique_lock lock(gate);
        CHECK(ready.wait_for(lock, std::chrono::seconds(10), [&] { return entered; }));
    }
    auto latest = first;
    for (int i = 0; i < 64; ++i) latest = worker.request();
    CHECK(latest > first && calls.load() == 1);
    PresetCatalogResult result;
    result.generation = 999;
    CHECK(!worker.poll(result) && result.generation == 999);
    {
        const std::lock_guard lock(gate);
        release = true;
    }
    ready.notify_one();
    CHECK(waitForCatalog(worker, result));
    CHECK(result.generation == latest && result.catalog.entries.size() == 1);
    if (result.catalog.entries.size() == 1) CHECK(result.catalog.entries[0].id == "user:latest");
    CHECK(cancelled.load() && !scannedOnCaller.load() && calls.load() == 2);
    CHECK(!worker.poll(result)); // each accepted result is delivered once

    // Exceptions are visible results, and the same worker can service a retry.
    int attempt = 0;
    PresetCatalogWorker retry([&](std::stop_token) {
        if (++attempt == 1) throw std::runtime_error("synthetic scan failure");
        return PresetCatalog{};
    });
    const auto failed = retry.request();
    CHECK(waitForCatalog(retry, result));
    CHECK(result.generation == failed && !result.catalog.error.empty());
    const auto retried = retry.request();
    CHECK(waitForCatalog(retry, result));
    CHECK(result.generation == retried && result.catalog.error.empty());

    // The real provider retains the existing entry and byte budget.
    TempDir td;
    const std::string text = cleanText();
    for (int i = 0; i < 3; ++i)
        std::ofstream(td.path / (std::to_string(i) + std::string(kPresetExtension))) << text;
    PresetCatalogWorker bounded(td.path, {1, kMaxCatalogReadBytes});
    const auto boundedGeneration = bounded.request();
    CHECK(waitForCatalog(bounded, result));
    CHECK(result.generation == boundedGeneration && result.catalog.truncated);
    CHECK(result.catalog.visited == 1 && result.catalog.bytesRead == text.size());
    CHECK(result.catalog.entries.size() == factoryPresets().size() + 1);

    // Shutdown cancels the active scan before joining and discards pending work.
    entered = false;
    std::atomic<bool> shutdownCancelled{false};
    {
        PresetCatalogWorker closing([&](std::stop_token stop) {
            std::stop_callback wake(stop, [&] { ready.notify_one(); });
            std::unique_lock lock(gate);
            entered = true;
            ready.notify_one();
            ready.wait_for(lock, std::chrono::seconds(10), [&] { return stop.stop_requested(); });
            shutdownCancelled.store(stop.stop_requested());
            return PresetCatalog{};
        });
        closing.request();
        std::unique_lock lock(gate);
        CHECK(ready.wait_for(lock, std::chrono::seconds(10), [&] { return entered; }));
    }
    CHECK(shutdownCancelled.load());
}
}  // namespace

int main() {
    testCatalogBudgets();
    testCatalogWorker();
    testFactory();
    testRoundtrip();
    testModified();
    testHostile();
    testSanitize();
    testLibrary();
    return TEST_RESULT("test_preset");
}
