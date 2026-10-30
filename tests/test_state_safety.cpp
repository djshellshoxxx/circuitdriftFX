// Regression coverage for reviewed state/JSON boundary failures. Synthetic data only.
#include "core/Json.h"
#include "state/StateDocument.h"
#include <atomic>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#if !defined(_WIN32)
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
using namespace cd;
int failures = 0;
#define CHECK(c) do { if (!(c)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

const char* kScene = R"({"schemaVersion":1,"product":"CircuitDrift","kind":"scene","engineModelVersion":"0.0.1-dev",
 "minReaderVersion":1,"author":"fixture","title":"Synthetic scene","tags":[],"createdAt":"2026-10-30T12:00:00Z",
 "requiredFeatures":["CD-F20"],"blueprint":{"schemaVersion":1,"id":"example.rc","components":[{"id":"R1","modelId":"fixture.resistor"}]},
 "baseParameters":{"hpf.frequency":20.5,"quality.oversampling":3},"macros":{},"timeline":{},"faults":[],"seed":42,
 "ui":{"filePath":"SYNTHETIC_PRIVATE_PATH"},"x-token":"SYNTHETIC_TOKEN"})";

core::JsonValue sceneTree() {
    core::JsonValue v; std::string error;
    CHECK(core::parseJson(kScene, v, error));
    return v;
}

std::string withSeed(const char* token) {
    std::string text = kScene;
    const auto at = text.find("\"seed\":42");
    text.replace(at, 9, std::string("\"seed\":") + token);
    return text;
}

void localeNumbers() {
    const std::string original = std::setlocale(LC_NUMERIC, nullptr);
    int commaLocales = 0;
    for (const char* candidate : {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "German_Germany.1252"}) {
        if (!std::setlocale(LC_NUMERIC, candidate)) continue;
        if (std::string(std::localeconv()->decimal_point) != ",") continue;
        ++commaLocales;
        core::JsonValue v; std::string error;
        CHECK(core::parseJson("[1.5,-2.75e-2,0.000001]", v, error));
        CHECK(v.array.size() == 3 && v.array[0].number == 1.5 && v.array[1].number == -0.0275);
        CHECK(core::writeJson(core::JsonValue::makeNumber(1.5)) == "1.5");
        CHECK(core::writeJson(core::JsonValue::makeNumber(0.1 + 0.2)) == "0.30000000000000004");
        for (double value : {0.0, -0.0, std::numeric_limits<double>::denorm_min(),
                             std::numeric_limits<double>::max(), -0.0275}) {
            CHECK(core::parseJson(core::writeJson(core::JsonValue::makeNumber(value)), v, error));
            CHECK(v.number == value && std::signbit(v.number) == std::signbit(value));
        }
        state::StateDocument doc, back;
        CHECK(state::loadState(kScene, doc, error));
        CHECK(state::loadState(state::saveState(doc), back, error));
        CHECK(back.baseParameters == doc.baseParameters && back.seed == 42);
        CHECK(std::string(std::localeconv()->decimal_point) == ","); // library never changes process locale
    }
    CHECK(std::setlocale(LC_NUMERIC, original.c_str()) != nullptr);
    if (!commaLocales) std::puts("SKIP comma-locale execution: no comma locale installed");
    else std::printf("comma-locale execution: %d locale selections passed\n", commaLocales);
    core::JsonValue v; std::string error;
    CHECK(!core::parseJson("1e-9999", v, error));
    CHECK(!core::parseJson("1e9999", v, error));
    bool threw = false;
    try { (void)core::writeJson(core::JsonValue::makeNumber(std::numeric_limits<double>::infinity())); }
    catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}

void exactIdentitiesAndFeatures() {
    state::StateDocument doc; std::string error;
    CHECK(state::loadState(kScene, doc, error));
    for (const char* token : {"9007199254740993", "9007199254740992.1", "9.007199254740993e15",
                             "42.000000000000000001", "4.2000000000000000001e1", "-1", "1e-9999"}) {
        const auto before = state::saveState(doc);
        CHECK(!state::loadState(withSeed(token), doc, error));
        CHECK(!error.empty() && state::saveState(doc) == before);
    }
    for (const auto& [token, expected] : {
            std::pair<const char*, std::uint64_t>{"9007199254740992", 9007199254740992ULL},
            {"9.007199254740992e15", 9007199254740992ULL}, {"90071992547409920e-1", 9007199254740992ULL},
            {"42.000e0", 42}, {"4200e-2", 42}, {"0e100000", 0}}) {
        CHECK(state::loadState(withSeed(token), doc, error));
        CHECK(doc.seed == expected);
    }
    CHECK(state::loadState(kScene, doc, error));
    auto tree = sceneTree();
    tree.get("baseParameters")->set(std::string("hpf.frequency\0alias", 19), core::JsonValue::makeNumber(100));
    CHECK(!state::loadState(core::writeJson(tree), doc, error));

    tree = sceneTree();
    tree.set("engineModelVersion", core::JsonValue::makeString("999.0.0"));
    const auto previous = state::saveState(doc);
    CHECK(!state::loadState(core::writeJson(tree), doc, error) && state::saveState(doc) == previous);
    tree = sceneTree();
    tree.get("requiredFeatures")->array.clear();
    for (int feature = 3; feature <= 25; ++feature) {
        char name[16]; std::snprintf(name, sizeof name, "CD-F%02d", feature);
        tree.get("requiredFeatures")->array.push_back(core::JsonValue::makeString(name));
    }
    CHECK(state::loadState(core::writeJson(tree), doc, error));
    tree.get("requiredFeatures")->array.push_back(core::JsonValue::makeString("CD-F99"));
    CHECK(!state::loadState(core::writeJson(tree), doc, error));

    CHECK(state::loadState(kScene, doc, error));
    doc.seed = 9007199254740993ULL;
    bool threw = false;
    try { (void)state::saveState(doc); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    doc.seed = 42;
    doc.kind = state::DocKind::Session;
    doc.checkpoint = state::EngineCheckpoint{"0.0.1-dev", "fixture-v1", 48000, 4, 9007199254740992ULL, {}, {}, {42}};
    const auto safe = state::saveState(doc);
    state::StateDocument back;
    CHECK(state::loadState(safe, back, error) && back.checkpoint->sampleIndex == 9007199254740992ULL);
    doc.checkpoint->engineModelVersion = "incompatible-checkpoint-version";
    CHECK(state::loadState(state::saveState(doc), back, error));
    const state::RuntimeIdentity runtime{"0.0.1-dev", "fixture-v1", 48000, 4, 0, 0};
    CHECK(state::decideRestore(back, runtime, error) == state::RestoreDecision::ResetWarm);
    std::string unsafe = safe;
    unsafe.replace(unsafe.find("\"sampleIndex\":9007199254740992"), 30, "\"sampleIndex\":9007199254740993");
    CHECK(!state::loadState(unsafe, back, error));
    doc.checkpoint->sampleIndex = 9007199254740993ULL;
    threw = false;
    try { (void)state::saveState(doc); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}

void manyObjectKeys() {
    std::string text = "{";
    constexpr int count = 20000;
    for (int i = 0; i < count; ++i) {
        if (i) text += ',';
        text += "\"key" + std::to_string(i) + "\":0";
    }
    text += '}';
    core::JsonValue tree; std::string error;
    CHECK(core::parseJson(text, tree, error) && tree.object.size() == count);
    text.pop_back();
    text += ",\"key0\":1}";
    CHECK(!core::parseJson(text, tree, error) && error.find("duplicate") != std::string::npos);
    CHECK(!core::parseJson(R"({"key0":0,"\u006bey0":1})", tree, error));
}

void publicationBoundary() {
    state::StateDocument doc; std::string error, out = "unchanged";
    CHECK(state::loadState(kScene, doc, error));
    CHECK(state::publishScene(doc, out, error));
    CHECK(out.find("SYNTHETIC_PRIVATE_PATH") == std::string::npos && out.find("SYNTHETIC_TOKEN") == std::string::npos);
    state::StateDocument back;
    CHECK(state::loadState(out, back, error) && back.baseParameters == doc.baseParameters);
    for (const char* location : {"macros", "timeline", "faults", "blueprint", "component"}) {
        auto tree = sceneTree();
        auto privateValue = core::JsonValue::makeString("SYNTHETIC_PRIVATE_DATA");
        if (std::string(location) == "component") tree.get("blueprint")->get("components")->array[0].set("credentials", privateValue);
        else if (std::string(location) == "faults") {
            tree.set("faults", core::JsonValue::makeObject());
            tree.get("faults")->set("capturedAudio", privateValue);
        } else tree.get(location)->set("filePath", privateValue);
        CHECK(state::loadState(core::writeJson(tree), doc, error));
        const auto local = state::saveState(doc);
        out = "unchanged";
        CHECK(!state::publishScene(doc, out, error) && out == "unchanged" && !error.empty());
        CHECK(state::saveState(doc) == local); // unsupported sound/content remains intact locally
    }
    CHECK(state::loadState(kScene, doc, error));
    doc.macros.set("age", core::JsonValue::makeNumber(0.5));
    out = "unchanged";
    CHECK(!state::publishScene(doc, out, error) && out == "unchanged"); // do not drop even benign unvalidated controls
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), {});
}

void atomicFiles() {
    namespace fs = std::filesystem;
    fs::path directory;
    // The test itself also claims its directory exclusively; never removes a preexisting path.
    for (unsigned n = 0; n < 10000; ++n) {
        auto candidate = fs::temp_directory_path() / ("circuitdrift-state-safety-" + std::to_string(n));
        if (fs::create_directory(candidate)) { directory = std::move(candidate); break; }
    }
    CHECK(!directory.empty());
    if (directory.empty()) return;
    const auto destination = directory / "scene.json";
    const auto victim = directory / "unrelated.txt";
    std::string error;
    CHECK(state::saveStateFileAtomic(victim.string(), "unrelated", error));
    const auto legacyTemporary = fs::path(destination.string() + ".tmp");
    std::error_code symlinkError;
    fs::create_symlink(victim, legacyTemporary, symlinkError);
    if (!symlinkError) {
        CHECK(state::saveStateFileAtomic(destination.string(), "first", error));
        CHECK(readFile(victim) == "unrelated" && readFile(destination) == "first" && fs::is_symlink(legacyTemporary));
        fs::remove(legacyTemporary);
        const auto destinationLink = directory / "destination-link.json";
        fs::create_symlink(victim, destinationLink);
        CHECK(state::saveStateFileAtomic(destinationLink.string(), "replacement", error));
        CHECK(readFile(victim) == "unrelated" && !fs::is_symlink(destinationLink) && readFile(destinationLink) == "replacement");
    } else std::puts("SKIP symlink regression: platform denied symlink creation");
    { std::ofstream file(legacyTemporary); file << "unrelated temp file"; }
    CHECK(state::saveStateFileAtomic(destination.string(), "second", error));
    CHECK(readFile(legacyTemporary) == "unrelated temp file" && readFile(destination) == "second");
    const auto blocked = directory / "existing-directory";
    fs::create_directory(blocked);
    { std::ofstream file(blocked / "keep"); file << "keep"; }
    CHECK(!state::saveStateFileAtomic(blocked.string(), "must not replace directory", error));
    CHECK(readFile(blocked / "keep") == "keep");
    CHECK(!state::saveStateFileAtomic(destination.string() + std::string("\0suffix", 7), "invalid", error));
    CHECK(readFile(destination) == "second");

    const std::string a(32768, 'a'), b(32768, 'b');
    CHECK(state::saveStateFileAtomic(destination.string(), a, error));
    std::atomic<int> pending{2}, errors{0};
    std::atomic<bool> start{false};
    auto writer = [&](const std::string& contents) {
        while (!start.load()) std::this_thread::yield();
        for (int i = 0; i < 12; ++i) {
            std::string why;
            if (!state::saveStateFileAtomic(destination.string(), contents, why)) ++errors;
        }
        --pending;
    };
    std::thread one(writer, std::cref(a)), two(writer, std::cref(b));
    start = true;
#if !defined(_WIN32)
    // POSIX permits replacement while the reader is open. Windows readers may
    // deny FILE_SHARE_DELETE, in which case a save must report failure instead.
    while (pending.load()) {
        const auto contents = readFile(destination);
        if (contents != a && contents != b) ++errors;
    }
#endif
    one.join(); two.join();
    CHECK(errors == 0);
    CHECK(readFile(destination) == a || readFile(destination) == b);

#if !defined(_WIN32)
    CHECK(state::saveStateFileAtomic(destination.string(), "preserved", error));
    const auto child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        std::signal(SIGXFSZ, SIG_IGN);
        const rlimit limit{1024, 1024};
        if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) ::_exit(2);
        std::string why;
        const bool success = state::saveStateFileAtomic(destination.string(), std::string(8192, 'x'), why);
        ::_exit(!success && !why.empty() ? 0 : 1);
    }
    if (child > 0) {
        int status = 0;
        CHECK(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        CHECK(readFile(destination) == "preserved");
    }
#endif
    for (const auto& entry : fs::directory_iterator(directory))
        CHECK(entry.path().filename().string().find(".tmp.") == std::string::npos);
    fs::remove_all(directory);
}
} // namespace

int main() {
    localeNumbers(); exactIdentitiesAndFeatures(); manyObjectKeys(); publicationBoundary(); atomicFiles();
    std::printf("state safety regressions: %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
