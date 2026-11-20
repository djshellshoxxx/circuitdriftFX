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

// A custom migration can introduce malformed in-memory strings even when the
// original document was valid UTF-8. Reject those trees without throwing.
bool migrateInvalidMetadataKey(core::JsonValue& tree, std::string&) {
    tree.set("schemaVersion", core::JsonValue::makeNumber(1));
    tree.set(std::string("\x80", 1), core::JsonValue::makeString("synthetic"));
    return true;
}

bool migrateInvalidMetadataValue(core::JsonValue& tree, std::string&) {
    tree.set("schemaVersion", core::JsonValue::makeNumber(1));
    tree.set("x-token", core::JsonValue::makeString(std::string("\x80", 1)));
    return true;
}

void migratedUtf8Boundary() {
    auto input = sceneTree();
    input.set("schemaVersion", core::JsonValue::makeNumber(0));
    const auto legacy = core::writeJson(input);
    state::StateDocument doc;
    std::string error;
    CHECK(state::loadState(kScene, doc, error));
    const auto before = state::saveState(doc);
    for (const state::MigrationFn step : {migrateInvalidMetadataKey, migrateInvalidMetadataValue}) {
        const state::MigrationChain migrations{0, {step}};
        bool rejected = false, threw = false;
        try { rejected = !state::loadState(legacy, doc, error, migrations); }
        catch (const std::invalid_argument&) { threw = true; }
        CHECK(!threw && rejected);
        CHECK(!error.empty());
        CHECK(state::saveState(doc) == before);
    }
}

void utf8WriterBoundary() {
    using core::JsonValue;
    const auto rejects = [](const JsonValue& value) {
        try { (void)core::writeJson(value); return false; }
        catch (const std::invalid_argument&) { return true; }
    };
    for (const std::string invalid : {
        std::string("\x80", 1), std::string("\xC0\xAF", 2),
        std::string("\xE0\x80\x80", 3), std::string("\xED\xA0\x80", 3),
        std::string("\xF4\x90\x80\x80", 4), std::string("\xF0\x9F", 2)}) {
        CHECK(rejects(JsonValue::makeString(invalid)));
        auto obj = JsonValue::makeObject();
        obj.set(invalid, JsonValue::makeNumber(1));
        CHECK(rejects(obj));
    }
    const std::string valid = std::string("A\xC3\xA9\xF0\x9F\x98\x80", 7);
    auto obj = JsonValue::makeObject();
    obj.set(valid, JsonValue::makeString(valid));
    std::string error;
    JsonValue parsed;
    CHECK(core::parseJson(core::writeJson(obj), parsed, error));
    CHECK(parsed.get(valid) && parsed.get(valid)->string == valid);
    CHECK(core::writeJson(JsonValue::makeString(std::string("nul\0byte", 8))) ==
          "\"nul\\u0000byte\"");
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
    // A matching engine/layout with incompatible RNG shape is not an exact
    // stochastic continuation. Programmatic checkpoints must also be finite.
    doc.checkpoint->engineModelVersion = "0.0.1-dev";
    state::RuntimeIdentity exact{"0.0.1-dev", "fixture-v1", 48000, 4, 0, 0, 1};
    CHECK(state::decideRestore(doc, exact, error) == state::RestoreDecision::Restore && error.empty());
    state::RuntimeIdentity wrongRng = exact;
    wrongRng.rngSize = 2;
    CHECK(state::decideRestore(doc, wrongRng, error) == state::RestoreDecision::ResetWarm);
    CHECK(error.find("rng") != std::string::npos);
    doc.checkpoint->electrical = {std::numeric_limits<double>::quiet_NaN()};
    exact.electricalSize = 1;
    CHECK(state::decideRestore(doc, exact, error) == state::RestoreDecision::ResetWarm);
    CHECK(error.find("invalid checkpoint") != std::string::npos);
    doc.checkpoint->electrical = {0.25};
    CHECK(state::decideRestore(doc, exact, error) == state::RestoreDecision::Restore);
    // In-memory checkpoints bypass the JSON parser's 4096/64 element limits.
    // Even a matching (but forged) prepared layout must not authorize restore.
    doc.checkpoint->electrical.assign(4097, 0.25);
    exact.electricalSize = doc.checkpoint->electrical.size();
    CHECK(state::decideRestore(doc, exact, error) == state::RestoreDecision::ResetWarm);
    CHECK(error.find("bounds") != std::string::npos);
    doc.checkpoint->electrical = {0.25};
    exact.electricalSize = 1;
    doc.checkpoint->thermal.assign(4097, 0.125);
    exact.thermalSize = doc.checkpoint->thermal.size();
    CHECK(state::decideRestore(doc, exact, error) == state::RestoreDecision::ResetWarm);
    CHECK(error.find("bounds") != std::string::npos);
    doc.checkpoint->thermal.clear();
    exact.thermalSize = 0;
    doc.checkpoint->rng.assign(65, 42);
    exact.rngSize = doc.checkpoint->rng.size();
    CHECK(state::decideRestore(doc, exact, error) == state::RestoreDecision::ResetWarm);
    CHECK(error.find("bounds") != std::string::npos);
    doc.checkpoint->rng = {42};
    exact.rngSize = 1;
    CHECK(state::decideRestore(doc, exact, error) == state::RestoreDecision::Restore);
    doc.checkpoint->sampleIndex = 9007199254740993ULL;
    CHECK(state::decideRestore(doc, exact, error) == state::RestoreDecision::ResetWarm);
    doc.checkpoint->sampleIndex = 9007199254740992ULL;
    exact.rngSize = 0;
    CHECK(state::decideRestore(doc, exact, error) == state::RestoreDecision::ResetWarm);
    doc.checkpoint.reset();
    CHECK(state::decideRestore(doc, exact, error) == state::RestoreDecision::NoCheckpoint);
    doc.checkpoint = state::EngineCheckpoint{"0.0.1-dev", "fixture-v1", 48000, 4, 9007199254740992ULL, {}, {}, {42}};
    std::string unsafe = safe;
    unsafe.replace(unsafe.find("\"sampleIndex\":9007199254740992"), 30, "\"sampleIndex\":9007199254740993");
    CHECK(!state::loadState(unsafe, back, error));
    doc.checkpoint->sampleIndex = 9007199254740993ULL;
    threw = false;
    try { (void)state::saveState(doc); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    // Session serialization must enforce the same checkpoint admission bounds
    // as loadState(), even for snapshots assembled without a JSON reader.
    doc.checkpoint->sampleIndex = 100;
    const auto goodCheckpoint = *doc.checkpoint;
    const auto rejectsSave = [&]() {
        try { (void)state::saveState(doc); return false; }
        catch (const std::invalid_argument&) { return true; }
    };
    doc.checkpoint->electrical.assign(4097, 0.25);
    CHECK(rejectsSave());
    doc.checkpoint = goodCheckpoint;
    doc.checkpoint->thermal.assign(4097, 0.25);
    CHECK(rejectsSave());
    doc.checkpoint = goodCheckpoint;
    doc.checkpoint->rng.assign(65, 42);
    CHECK(rejectsSave());
    doc.checkpoint = goodCheckpoint;
    doc.checkpoint->electrical = {std::numeric_limits<double>::quiet_NaN()};
    CHECK(rejectsSave());
    doc.checkpoint = goodCheckpoint;
    doc.checkpoint->thermal = {std::numeric_limits<double>::infinity()};
    CHECK(rejectsSave());
    doc.checkpoint = goodCheckpoint;
    doc.checkpoint->sampleRate = std::numeric_limits<double>::quiet_NaN();
    CHECK(rejectsSave());
    doc.checkpoint = goodCheckpoint;
    doc.checkpoint->oversampling = 3;
    CHECK(rejectsSave());
    doc.checkpoint = goodCheckpoint;
    doc.checkpoint->modelRevision.clear();
    CHECK(rejectsSave());
    doc.checkpoint = goodCheckpoint;
    doc.kind = state::DocKind::Scene;
    CHECK(rejectsSave());
    doc.kind = state::DocKind::Session;
    CHECK(state::loadState(state::saveState(doc), back, error));
}

void checkpointAdmissionBoundary() {
    state::StateDocument doc;
    std::string error;
    CHECK(state::loadState(kScene, doc, error));
    doc.kind = state::DocKind::Session;
    doc.checkpoint = state::EngineCheckpoint{"0.0.1-dev", "fixture-v1", 48000, 4, 100,
                                             {0.25}, {0.5}, {42}};
    const auto original = doc;
    const state::RuntimeIdentity runtime{"0.0.1-dev", "fixture-v1", 48000, 4, 1, 1, 1};
    CHECK(state::decideRestore(doc, runtime, error) == state::RestoreDecision::Restore && error.empty());

    // Programmatic checkpoints must retain the session-only and bounded identity
    // admission rules enforced by both the reader and the local writer.
    for (const auto kind : {state::DocKind::Scene, static_cast<state::DocKind>(255)}) {
        doc.kind = kind;
        CHECK(state::decideRestore(doc, runtime, error) == state::RestoreDecision::ResetWarm);
        CHECK(!error.empty());
    }
    for (const std::string identity : {std::string{}, std::string(65, 'x'), std::string("\x80", 1)}) {
        for (const bool engineIdentity : {false, true}) {
            doc = original;
            auto matchingRuntime = runtime;
            if (engineIdentity) {
                doc.checkpoint->engineModelVersion = identity;
                matchingRuntime.engineModelVersion = identity;
            } else {
                doc.checkpoint->modelRevision = identity;
                matchingRuntime.modelRevision = identity;
            }
            CHECK(state::decideRestore(doc, matchingRuntime, error) == state::RestoreDecision::ResetWarm);
            CHECK(!error.empty());
        }
    }

    // Inclusive schema limits must remain valid and lossless, including signed
    // zero, finite extremes, subnormal state values and all 64 bits of RNG state.
    for (const double rate : {8000.0, 768000.0}) {
        doc = original;
        auto& cp = *doc.checkpoint;
        cp.engineModelVersion.assign(64, 'e');
        cp.modelRevision.assign(64, 'r');
        if (rate == 768000.0) cp.modelRevision.replace(62, 2, "\xc3\xa9");
        cp.sampleRate = rate;
        cp.oversampling = rate == 8000.0 ? 1 : 16;
        cp.sampleIndex = 9007199254740992ULL;
        cp.electrical.assign(4096, std::numeric_limits<double>::denorm_min());
        cp.electrical[0] = -0.0;
        cp.electrical[1] = std::numeric_limits<double>::max();
        cp.thermal.assign(4096, -std::numeric_limits<double>::denorm_min());
        cp.thermal[0] = 0.0;
        cp.thermal[1] = -std::numeric_limits<double>::max();
        cp.rng.assign(64, std::numeric_limits<std::uint64_t>::max());
        cp.rng[0] = 0;
        const state::RuntimeIdentity exact{cp.engineModelVersion, cp.modelRevision, rate, cp.oversampling,
                                            cp.electrical.size(), cp.thermal.size(), cp.rng.size()};
        const auto encoded = state::saveState(doc);
        state::StateDocument back;
        CHECK(state::loadState(encoded, back, error));
        CHECK(back.checkpoint.has_value());
        if (!back.checkpoint) continue;
        const auto& restored = *back.checkpoint;
        CHECK(restored.engineModelVersion == cp.engineModelVersion && restored.modelRevision == cp.modelRevision);
        CHECK(restored.sampleRate == cp.sampleRate && restored.oversampling == cp.oversampling);
        CHECK(restored.sampleIndex == cp.sampleIndex && restored.rng == cp.rng);
        CHECK(restored.electrical == cp.electrical && restored.thermal == cp.thermal);
        CHECK(std::signbit(restored.electrical[0]) && !std::signbit(restored.thermal[0]));
        CHECK(state::saveState(back) == encoded);
        CHECK(state::decideRestore(back, exact, error) == state::RestoreDecision::Restore && error.empty());
    }
}

// Directly assembled documents must not generate local files the v1 reader
// cannot load, or silently erase/replace fields during serialization.
void wholeDocumentWriterBoundary() {
    state::StateDocument doc, back;
    std::string error;
    CHECK(state::loadState(kScene, doc, error));
    const auto original = doc;
    const auto baseline = state::saveState(doc);
    const auto rejects = [&]() {
        try { (void)state::saveState(doc); return false; }
        catch (const std::invalid_argument&) { return true; }
    };
    doc.createdAt = "not-a-date";
    CHECK(rejects());
    doc = original;
    doc.engineModelVersion = "unsupported-engine";
    CHECK(rejects());
    doc = original;
    doc.title.assign(257, 'x');
    CHECK(rejects());
    doc = original;
    doc.requiredFeatures.push_back("CD-F99");
    CHECK(rejects());
    doc = original;
    doc.baseParameters.emplace_back("hpf.frequency", 42.0);
    CHECK(rejects());
    doc = original;
    doc.baseParameters.emplace_back("not.a.registered.param", 0.0);
    CHECK(rejects());
    doc = original;
    doc.baseParameters[0].second = std::numeric_limits<double>::infinity();
    CHECK(rejects());
    doc = original;
    doc.metadata.emplace_back("title", core::JsonValue::makeString("override"));
    CHECK(rejects());
    doc = original;
    doc.metadata.emplace_back("x-token", core::JsonValue::makeString("duplicate"));
    CHECK(rejects());
    doc = original;
    doc.metadata.emplace_back("x-large", core::JsonValue::makeString(std::string(65536, 'x')));
    CHECK(rejects());
    doc = original;
    doc.blueprints[0].get("components")->array[0].set("modelId", core::JsonValue::makeString("not-a-fixture"));
    CHECK(rejects());
    // A scene with an extra (A/B) blueprint used to serialize successfully
    // while silently discarding every blueprint after the first.
    doc = original;
    doc.blueprints.push_back(doc.blueprints.front());
    CHECK(rejects());
    doc = original;
    doc.blueprints.clear();
    CHECK(rejects());
    doc = original;
    doc.kind = static_cast<state::DocKind>(255);
    CHECK(rejects());
    // Legitimate 2-slot sessions must still persist both blueprints.
    doc = original;
    doc.kind = state::DocKind::Session;
    doc.blueprints.push_back(doc.blueprints.front());
    const auto twoSlotSession = state::saveState(doc);
    CHECK(state::loadState(twoSlotSession, back, error));
    CHECK(back.kind == state::DocKind::Session && back.blueprints.size() == 2);
    doc.blueprints.push_back(doc.blueprints.front());
    CHECK(rejects());
    doc = original;
    doc.macros = core::JsonValue::makeArray();
    CHECK(rejects());
    doc = original;
    doc.timeline = core::JsonValue::makeNumber(1);
    CHECK(rejects());
    doc = original;
    doc.faults = core::JsonValue::makeString("lost");
    CHECK(rejects());
    doc = original;
    doc.ui = core::JsonValue::makeArray();
    CHECK(rejects());
    doc = original;
    CHECK(state::saveState(doc) == baseline);
    CHECK(state::loadState(state::saveState(doc), back, error));
    CHECK(back.metadata.size() == original.metadata.size());
    CHECK(!back.metadata.empty() && back.metadata[0].first == "x-token" &&
          back.metadata[0].second.isString() && back.metadata[0].second.string == "SYNTHETIC_TOKEN");
}

void writerPayloadLimits() {
    using core::JsonValue;
    state::StateDocument doc, back;
    std::string error;
    CHECK(state::loadState(kScene, doc, error));
    const auto original = doc;
    const auto rejects = [&]() {
        try { (void)state::saveState(doc); return false; }
        catch (const std::invalid_argument&) { return true; }
    };
    const auto nested = [](int containers) {
        auto value = JsonValue::makeString("synthetic");
        for (int i = 0; i < containers; ++i) {
            auto parent = JsonValue::makeObject();
            parent.set("child", std::move(value));
            value = std::move(parent);
        }
        return value;
    };

    // The document root counts toward depth 20, as does the extra blueprints
    // array in a session. Exercise only the exact limit and one level beyond it.
    doc.macros = nested(19);
    CHECK(state::loadState(state::saveState(doc), back, error));
    CHECK(core::writeJson(back.macros) == core::writeJson(doc.macros));
    doc.macros = nested(20);
    CHECK(rejects());
    for (const auto kind : {state::DocKind::Scene, state::DocKind::Session}) {
        doc = original;
        doc.kind = kind;
        const int payloadDepth = kind == state::DocKind::Scene ? 18 : 17;
        doc.blueprints[0].set("extension", nested(payloadDepth));
        CHECK(state::loadState(state::saveState(doc), back, error));
        CHECK(core::writeJson(back.blueprints[0]) == core::writeJson(doc.blueprints[0]));
        doc.blueprints[0].set("extension", nested(payloadDepth + 1));
        CHECK(rejects());
    }
    doc = original;
    doc.metadata.emplace_back("x-depth", nested(19));
    CHECK(state::loadState(state::saveState(doc), back, error));
    doc.metadata.back().second = nested(20);
    CHECK(rejects());
    doc = original;
    auto malformed = JsonValue::makeString("visible");
    malformed.array.push_back(JsonValue::makeString("inactive"));
    doc.metadata.emplace_back("x-value", std::move(malformed));
    CHECK(rejects());

    // Inclusive byte limits remain readable, and adding one byte is rejected.
    // Use ordinary bounded fixtures rather than large-depth/stack tests.
    for (const auto kind : {state::DocKind::Scene, state::DocKind::Session}) {
        doc = original;
        doc.kind = kind;
        doc.macros.set("padding", JsonValue::makeString(""));
        const auto overhead = state::saveState(doc).size();
        const auto limit = kind == state::DocKind::Scene ? state::kSceneMaxBytes : state::kSessionMaxBytes;
        doc.macros.get("padding")->string.assign(limit - overhead, 'x');
        const auto encoded = state::saveState(doc);
        CHECK(encoded.size() == limit && state::loadState(encoded, back, error));
        doc.macros.get("padding")->string.push_back('x');
        CHECK(rejects());
    }
    doc = original;
    doc.blueprints[0].set("padding", JsonValue::makeString(""));
    const auto blueprintOverhead = core::writeJson(doc.blueprints[0]).size();
    doc.blueprints[0].get("padding")->string.assign(state::kBlueprintMaxBytes - blueprintOverhead, 'x');
    CHECK(state::loadState(state::saveState(doc), back, error));
    doc.blueprints[0].get("padding")->string.push_back('x');
    CHECK(rejects());

    // Metadata accounts for raw key bytes and encoded value bytes. Escaped
    // controls therefore test both the writer byte count and reader symmetry.
    doc = original;
    doc.metadata.clear();
    const std::string key = "x-\n";
    const auto paddingBytes = state::kMetadataMaxBytes - key.size() - 2;
    std::string padding(paddingBytes / 6, '\n');
    padding.append(paddingBytes % 6, 'x');
    doc.metadata.emplace_back(key, JsonValue::makeString(std::move(padding)));
    CHECK(state::loadState(state::saveState(doc), back, error));
    CHECK(back.metadata.size() == 1 && back.metadata[0].first == key);
    doc.metadata[0].second.string.push_back('x');
    CHECK(rejects());
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
    CHECK(state::loadState(kScene, doc, error));
    doc.baseParameters.emplace_back("hpf.frequency", 200.0);
    out = "unchanged";
    CHECK(!state::publishScene(doc, out, error) && out == "unchanged" && !error.empty());
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
    migratedUtf8Boundary();
    utf8WriterBoundary();
    localeNumbers(); exactIdentitiesAndFeatures(); checkpointAdmissionBoundary(); wholeDocumentWriterBoundary(); writerPayloadLimits(); manyObjectKeys(); publicationBoundary(); atomicFiles();
    std::printf("state safety regressions: %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
