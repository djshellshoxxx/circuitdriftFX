// Tests: T08 bundled tooltip catalogue for every shipped (SYNTHETIC fixture) model + 450 ms timer.
#include <cstdint>
#include <cstdio>
#include <string>
#include "uimodel/Tooltip.h"

namespace {
using namespace cd;
using namespace cd::uimodel;
int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d [%s]: ", __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

void testCatalogue() {
    const auto reg = blueprint::ModelRegistry::syntheticFixtures();
    CHECK(!reg.devices.empty() && !reg.sources.empty(), "registry populated");
    std::size_t shipped = 0;
    for (const auto& d : reg.devices) {
        ++shipped;
        const TooltipEntry* e = findTooltip(d.modelId, d.revision);
        CHECK(e != nullptr, "tooltip for %s r%d", d.modelId.c_str(), d.revision);
        if (!e) continue;
        CHECK(validateEntry(*e).empty(), "%s: %s", d.modelId.c_str(), validateEntry(*e).c_str());
        CHECK(std::string(e->units) == unitsFor(d), "%s units '%s' vs manifest '%s'", d.modelId.c_str(), e->units, unitsFor(d).c_str());
        CHECK(d.provenance == dsp::ProvenanceKind::SyntheticFixture, "fixtures are synthetic");
        const std::string role = e->role;
        switch (d.equation) {
            case dsp::DeviceEquation::Resistor: CHECK(role.find("resistance") != std::string::npos, "resistor role"); break;
            case dsp::DeviceEquation::Capacitor: CHECK(role.find("capacitance") != std::string::npos, "capacitor role"); break;
            case dsp::DeviceEquation::ShockleyDiode: CHECK(role.find("junction") != std::string::npos, "diode role"); break;
        }
        CHECK(std::string(e->material).find("Not modelled") == 0, "%s material honest", d.modelId.c_str());
        // A germanium diode appears only if a supported model contains one: fixtures must not claim it.
        CHECK(std::string(e->longText).find("no germanium part is represented") != std::string::npos || d.equation != dsp::DeviceEquation::ShockleyDiode, "no germanium claim");
    }
    for (const auto& s : reg.sources) {
        ++shipped;
        const TooltipEntry* e = findTooltip(s.modelId, s.revision);
        CHECK(e != nullptr, "tooltip for source %s", s.modelId.c_str());
        if (!e) continue;
        CHECK(validateEntry(*e).empty(), "%s: %s", s.modelId.c_str(), validateEntry(*e).c_str());
        CHECK(std::string(e->units) == unitsFor(s), "source units");
    }
    CHECK(tooltipCatalogue().size() == shipped, "no orphan entries (%zu vs %zu)", tooltipCatalogue().size(), shipped);
    for (const auto& e : tooltipCatalogue()) {
        CHECK(std::string(e.sourceLink).find("http") == std::string::npos, "%s: no network link", e.modelId);
        CHECK(wordCount(e.shortText) <= 60 && wordCount(e.longText) <= 250, "%s word limits (%d/%d)", e.modelId, wordCount(e.shortText), wordCount(e.longText));
        CHECK(std::string(e.provenance).find("ynthetic fixture") != std::string::npos, "%s provenance", e.modelId);
    }
    CHECK(findTooltip("fixture.unknown") == nullptr && findTooltip("fixture.diode", 99) == nullptr, "unknown -> null");
    // negative controls for the validator
    TooltipEntry bad = tooltipCatalogue()[0];
    bad.provenance = "measured";
    CHECK(!validateEntry(bad).empty(), "provenance check bites");
    bad = tooltipCatalogue()[0];
    bad.sourceLink = "https://example.com";
    CHECK(!validateEntry(bad).empty(), "offline check bites");
    bad = tooltipCatalogue()[0];
    bad.units = "";
    CHECK(!validateEntry(bad).empty(), "empty field check bites");
    CHECK(wordCount("") == 0 && wordCount("  a  b\tc\n") == 3 && wordCount(nullptr) == 0, "wordCount");
}

void testTimer() {
    const std::int64_t frame = 16667;
    for (int phase : {0, 5000, 16000}) {
        std::int64_t now = 123456789;
        TooltipTimer t([&] { return now; });
        CHECK(t.tick() == TooltipTimer::State::Idle, "idle");
        now += phase;
        const std::int64_t t0 = now;
        t.start("R1");
        CHECK(t.state() == TooltipTimer::State::Pending, "pending");
        std::int64_t openedAt = -1;
        for (int i = 0; i < 60 && openedAt < 0; ++i) {
            now += frame;
            if (t.tick() == TooltipTimer::State::Open) openedAt = now - t0;
        }
        CHECK(openedAt >= 450000, "never opens early (%lld us)", (long long)openedAt);
        CHECK(openedAt < 450000 + frame, "opens within one frame of 450 ms (%lld us)", (long long)openedAt);
    }
    std::int64_t now = 0;
    TooltipTimer t([&] { return now; });
    t.start("A");
    now = 449999;
    CHECK(t.tick() == TooltipTimer::State::Pending, "449.999 ms still pending");
    now = 450000;
    CHECK(t.tick() == TooltipTimer::State::Open, "exactly 450 ms opens");
    t.leave();
    CHECK(t.state() == TooltipTimer::State::Idle, "leave closes");
    // re-targeting restarts the delay; hovering the same target does not
    now = 1000000; t.start("A"); now += 300000; t.start("A"); now += 200000;
    CHECK(t.tick() == TooltipTimer::State::Open, "same target keeps its timer");
    t.leave();
    now += 10; t.start("A"); now += 300000; t.start("B"); now += 200000;
    CHECK(t.tick() == TooltipTimer::State::Pending && t.target() == "B", "new target restarts");
    // Escape dismisses until leave; keyboard focus uses the same path
    now += 300000;
    CHECK(t.tick() == TooltipTimer::State::Open, "open");
    t.escape();
    CHECK(t.tick() == TooltipTimer::State::Dismissed, "Escape dismisses");
    now += 900000;
    CHECK(t.tick() == TooltipTimer::State::Dismissed, "stays dismissed");
    t.leave();
    CHECK(t.state() == TooltipTimer::State::Idle, "reset");
    // pin (touch)
    t.start("C"); now += 450000; t.tick();
    t.pin();
    t.leave();
    CHECK(t.state() == TooltipTimer::State::Open && t.pinned(), "pinned stays");
    t.escape();
    CHECK(t.state() == TooltipTimer::State::Dismissed && !t.pinned(), "Escape unpins");
    // pin only applies to an open tooltip
    t.leave(); t.start("D"); t.pin();
    CHECK(!t.pinned(), "cannot pin while pending");
}
}  // namespace

int main() {
    testCatalogue();
    testTimer();
    if (g_fail) { std::printf("uimodel tooltip: %d FAILED\n", g_fail); return 1; }
    std::printf("uimodel tooltip: all T08 checks passed\n");
    return 0;
}
