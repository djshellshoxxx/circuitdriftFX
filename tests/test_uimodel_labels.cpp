// Tests: UX-08 label audit (quality / latency / draft / Zero Latency / HPF / utility ranges).
// Every quality x family x zero-latency combination is enumerated through the engine's public
// resolveQuality; active vs pending is exercised with a real Engine (staged request). View-model
// only; nothing here claims physical validation. Owner: uimodel agent.
#include <cstdio>
#include <memory>
#include <string>
#include "runtime/Engine.h"
#include "uimodel/Labels.h"

namespace {
using namespace cd;
using namespace cd::runtime;
using namespace cd::uimodel;
int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d [%s]: ", __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

// T-UX08-a: exhaustive combinations.
void enumerateAll() {
    int combos = 0;
    const QualityMode modes[] = {QualityMode::Eco, QualityMode::Studio, QualityMode::Render, QualityMode::Manual};
    const int factors[] = {1, 2, 4, 8, 16};
    const dsp::OsFamily fams[] = {dsp::OsFamily::Fir, dsp::OsFamily::Iir};
    for (QualityMode m : modes)
        for (int f : factors)
            for (dsp::OsFamily fam : fams)
                for (bool zl : {false, true}) {
                    if (m != QualityMode::Manual && (f != 4 || fam != dsp::OsFamily::Fir)) continue;  // only Manual reads them
                    QualitySettings qs;
                    qs.mode = m; qs.manualFactor = f; qs.family = fam; qs.zeroLatency = zl;
                    dsp::Oversampler::Spec spec;
                    QualityInfo q;
                    CHECK(Engine::resolveQuality(qs, 0, spec, q), "resolve failed mode %d f %d", int(m), f);
                    q.latencySamples = zl ? 0 : 100;
                    const QualityLabel l = describeQuality(q, 48000.0);
                    ++combos;
                    const bool expectDraft = q.factor < 4 || zl;
                    CHECK(l.draft == expectDraft && (l.draftBadge == "Draft") == expectDraft && l.draftNote.empty() == !expectDraft,
                          "draft flag mode %d f %d zl %d", int(m), f, int(zl));
                    CHECK(has(l.full, "Draft") == expectDraft, "full line draft mode %d f %d zl %d: %s", int(m), f, int(zl), l.full.c_str());
                    CHECK(l.title == qualityLabel(q), "title must equal the engine label");
                    CHECK(has(l.title, "Draft") == expectDraft, "engine label vs draft: %s", l.title.c_str());
                    const bool iir = q.family == dsp::OsFamily::Iir;
                    CHECK(l.family == (iir ? "IIR" : "FIR"), "family");
                    CHECK((l.familyNote == "IIR low-latency exception") == iir && (has(l.full, "low-latency exception") == iir),
                          "IIR exception mode %d f %d zl %d: %s", int(m), f, int(zl), l.full.c_str());
                    CHECK(l.oversampling == (q.factor == 1 ? "Off (1x)" : std::to_string(q.factor) + "x"), "oversampling name %s", l.oversampling.c_str());
                    CHECK(zl == !l.unavailable.empty() && zl == has(l.full, "Unavailable:"), "zero latency list");
                    if (zl) {
                        bool pitch = false, lookahead = false, xtalk = false;
                        for (const auto& u : l.unavailable) {
                            pitch |= has(u, "Pitch Drift Delay"); lookahead |= has(u, "lookahead"); xtalk |= has(u, "Cross-talk");
                        }
                        CHECK(pitch && lookahead && xtalk && l.draft, "zero latency must list pitch delay/lookahead/cross-talk and be Draft");
                        CHECK(has(l.latency, "0 samples"), "latency text %s", l.latency.c_str());
                    }
                    CHECK(has(l.latency, "Latency"), "latency text");
                }
    CHECK(combos == 3 * 2 + 5 * 2 * 2, "combination count %d", combos);
    std::printf("UX-08 enumerated %d quality/family/zero-latency combinations\n", combos);
}

// pitch delay requested in Zero Latency is reported unavailable, otherwise no note.
void pitchNote() {
    QualityInfo q;
    q.factor = 1; q.draft = true; q.zeroLatency = true; q.pitchDelayUnavailable = true;
    CHECK(has(describeQuality(q, 48000).pitchNote, "unavailable in Zero Latency"), "pitch note");
    q.pitchDelayUnavailable = false;
    CHECK(describeQuality(q, 48000).pitchNote.empty(), "no pitch note");
    CHECK(has(describeQuality(q, 0.0).latency, "0.00 ms"), "non-positive rate must not divide by zero");
}

// Active vs pending vs incoming are distinct, using a real Engine.
EngineConfig config(QualityMode m, bool zl = false, int manual = 4, dsp::OsFamily fam = dsp::OsFamily::Fir) {
    EngineConfig c;
    c.sampleRate = 48000; c.maxBlock = 512; c.channels = 2;
    c.quality.mode = m; c.quality.manualFactor = manual; c.quality.family = fam; c.quality.zeroLatency = zl;
    c.circuit = syntheticDiodeClipper();
    return c;
}

void activeVsPending() {
    std::string e;
    Engine eng;
    CHECK(eng.prepare(config(QualityMode::Studio), e), "prepare: %s", e.c_str());
    {
        const QualityStatusLabel s = describeQualityStatus(eng.qualityStatus(), 48000);
        CHECK(!s.hasPending && !s.hasIncoming && s.summary == s.activeLine && has(s.activeLine, "Active: Studio 4x FIR"), "idle: %s", s.summary.c_str());
    }
    // Zero Latency changes latency: staged -> pending, active unchanged.
    auto cand = eng.buildPrepared(config(QualityMode::Studio, true), e);
    CHECK(cand != nullptr, "build: %s", e.c_str());
    CHECK(eng.requestQuality(cand) == QualityRequest::Staged, "staged");
    const QualityStatusLabel s = describeQualityStatus(eng.qualityStatus(), 48000);
    CHECK(s.hasPending && !s.hasIncoming, "pending flags");
    CHECK(s.active.full != s.pending.full && !s.active.draft && s.pending.draft, "active and pending must differ");
    CHECK(has(s.pendingLine, "Pending") && !has(s.activeLine, "Pending") && !has(s.activeLine, "Unavailable") && has(s.pendingLine, "Unavailable"),
          "pending line must not read as active: %s", s.summary.c_str());
    CHECK(has(s.summary, "Zero Latency 1x (Draft)"), "summary %s", s.summary.c_str());
    // Crossfading request: incoming is shown as switching, never as active.
    Engine eng2;
    CHECK(eng2.prepare(config(QualityMode::Studio), e), "prepare2");
    auto c2 = eng2.buildPrepared(config(QualityMode::Eco), e);
    CHECK(c2 && eng2.requestQuality(c2) == QualityRequest::Crossfading, "crossfade request");
    const QualityStatusLabel t = describeQualityStatus(eng2.qualityStatus(), 48000);
    CHECK(t.hasIncoming && !t.hasPending && has(t.incomingLine, "Switching to: Eco 4x FIR") && has(t.activeLine, "Studio 4x FIR"),
          "incoming: %s", t.summary.c_str());
    // After the host reactivates, the pending label becomes the active one.
    CHECK(eng.activatePending(e), "activate: %s", e.c_str());
    const QualityStatusLabel u = describeQualityStatus(eng.qualityStatus(), 48000);
    CHECK(!u.hasPending && has(u.activeLine, "Zero Latency 1x (Draft)") && u.active.draft, "after activation: %s", u.summary.c_str());
    // IIR is reported as the low-latency exception on the real engine too.
    Engine eng3;
    CHECK(eng3.prepare(config(QualityMode::Manual, false, 4, dsp::OsFamily::Iir), e), "prepare iir");
    CHECK(has(describeQualityStatus(eng3.qualityStatus(), 48000).activeLine, "IIR low-latency exception"), "iir label");
}

void hpfAndRanges() {
    CHECK(hpfLabel(false, 15) == "HPF off - DC passes" && hpfLabel(false, 250) == "HPF off - DC passes", "bypassed HPF must say DC passes");
    CHECK(hpfLabel(true, 15) == "HPF 15 Hz" && hpfLabel(true, 12.5) == "HPF 12.5 Hz" && hpfLabel(true, 15).find("DC passes") == std::string::npos, "active HPF");
    CHECK(utilityRange("input.gain").text == "\xC2\xB1" "24 dB", "input range %s", utilityRange("input.gain").text.c_str());
    CHECK(utilityRange("output.gain").text == "\xC2\xB1" "24 dB", "output range");
    CHECK(utilityRange("mix").text == "0\xE2\x80\x93" "100 %", "mix range %s", utilityRange("mix").text.c_str());
    CHECK(utilityRange("hpf.frequency").text == "10\xE2\x80\x93" "250 Hz", "hpf range %s", utilityRange("hpf.frequency").text.c_str());
    CHECK(utilityRange("input.gain").min == -24.0 && utilityRange("input.gain").max == 24.0 && utilityRange("hpf.frequency").min == 10.0 &&
              utilityRange("hpf.frequency").max == 250.0, "numeric ranges are the registry's");
    CHECK(utilityRange("macro.sag").id.empty() && utilityRange("nope").id.empty(), "non-utility ids");
    CHECK(utilityRanges().size() == 4, "list");
    const core::ParamSpec* hpf = core::findById("hpf.frequency");
    CHECK(hpf && hpf->def == 15.0, "HPF default 15 Hz");
}

}  // namespace

int main() {
    enumerateAll();
    pitchNote();
    activeVsPending();
    hpfAndRanges();
    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("uimodel labels: OK\n");
    return 0;
}
