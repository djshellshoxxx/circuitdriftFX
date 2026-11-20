// Plugin-side consistency test: the JUCE parameter tree must mirror the core registry exactly and
// the host-visible parameter ids must stay stable (golden table, see ADR-0001).
#include "ParameterLayout.h"
#include "PluginProcessor.h"
#include "ProcessorRegressions.h"
#include "core/ParameterRegistry.h"
#include "ui/GestureRegressions.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>

namespace {

int g_failures = 0;
#define CHECK(cond, ...)                                                          \
    do {                                                                          \
        if (!(cond)) {                                                            \
            ++g_failures;                                                         \
            std::fprintf(stderr, "FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond); \
            std::fprintf(stderr, __VA_ARGS__);                                    \
            std::fprintf(stderr, "\n");                                           \
        }                                                                         \
    } while (0)

struct GoldenRow {
    const char* id;
    std::uint32_t hostId;
    std::uint32_t hash;
};
constexpr GoldenRow kGolden[] = {
#include "ParamIdGolden.generated.inc"
};
constexpr std::size_t kGoldenCount = sizeof(kGolden) / sizeof(kGolden[0]);

using cd::core::ParamKind;
using cd::core::ParamSpec;
using cd::plugin::Edition;

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

void testGoldenTable() {
    CHECK(kGoldenCount == cd::core::kParamCount, "golden has %zu rows, registry %zu", kGoldenCount,
          cd::core::kParamCount);
    std::set<std::string> ids;
    std::set<std::uint32_t> hostIds, hashes, maskedHashes;
    for (std::size_t i = 0; i < kGoldenCount && i < cd::core::kParamCount; ++i) {
        const auto& g = kGolden[i];
        const auto& p = cd::core::kParams[i];
        CHECK(std::strcmp(g.id, p.id) == 0, "row %zu: golden id %s vs registry %s", i, g.id, p.id);
        CHECK(g.hostId == p.hostId, "%s: golden hostId %u vs registry %u", g.id, g.hostId, p.hostId);
        // Extension IDs 1000-1999; base-engine slot controls (CD-F20) use the disjoint range 1-999.
        const bool baseRange = p.hostId >= 1 && p.hostId < 1000 && std::strcmp(p.featureId, "CD-F20") == 0;
        CHECK(baseRange || (p.hostId >= 1000 && p.hostId <= 1999), "%s: hostId %u outside permanent ranges",
              p.id, p.hostId);
        // The host id the VST3/CLAP wrappers derive from the string id (ADR-0001) must not drift.
        const auto hash = static_cast<std::uint32_t>(juce::String(p.id).hashCode());
        CHECK(hash == g.hash, "%s: juce hash 0x%08X vs golden 0x%08X", p.id, hash, g.hash);
        CHECK(ids.insert(p.id).second, "duplicate string id %s", p.id);
        CHECK(hostIds.insert(p.hostId).second, "duplicate hostId %u", p.hostId);
        CHECK(hashes.insert(hash).second, "hash collision at %s", p.id);
        CHECK(maskedHashes.insert(hash & 0x7fffffffu).second, "31-bit hash collision at %s", p.id);
    }
}

void testEdition(Edition edition) {
    const bool lite = edition == Edition::Lite;
    cd::plugin::CircuitDriftProcessor proc(edition, lite ? "CircuitDrift LITE" : "CircuitDrift");

    std::map<std::string, juce::RangedAudioParameter*> byId;
    for (auto* p : proc.getParameters())
        if (auto* r = dynamic_cast<juce::RangedAudioParameter*>(p))
            CHECK(byId.emplace(r->paramID.toStdString(), r).second, "duplicate juce id %s",
                  r->paramID.toRawUTF8());

    const std::size_t expected = cd::plugin::exposedParameterCount(edition) + (lite ? 1 : 0);
    CHECK(proc.getParameters().size() == static_cast<int>(expected), "param count %d vs %zu",
          proc.getParameters().size(), expected);
    CHECK(byId.size() == expected, "ranged param count %zu vs %zu", byId.size(), expected);
    CHECK(lite ? proc.getBypassParameter() != nullptr : proc.getBypassParameter() == nullptr, "bypass parameter");
    CHECK(lite == (byId.count(cd::plugin::kBypassParamId) == 1), "bypass id presence");

    std::size_t seen = 0;
    for (const auto& spec : cd::core::kParams) {
        const bool isLiteId = std::strncmp(spec.id, "lite.", 5) == 0;
        const auto it = byId.find(spec.id);
        if (isLiteId != lite) {
            CHECK(it == byId.end(), "%s leaked into %s edition", spec.id, lite ? "LITE" : "full");
            continue;
        }
        CHECK(it != byId.end(), "%s missing", spec.id);
        if (it == byId.end()) continue;
        ++seen;
        auto* param = it->second;
        CHECK(param->isAutomatable() == spec.automatable, "%s automatable", spec.id);
        CHECK(near(param->getDefaultValue(), cd::core::toNormalized(spec, spec.def), 1e-5), "%s default", spec.id);
        CHECK(near(param->getNormalisableRange().start, spec.min, 1e-6) &&
                  near(param->getNormalisableRange().end, spec.max, 1e-6),
              "%s range", spec.id);
        CHECK(param->getVersionHint() == cd::plugin::kParameterVersionHint, "%s version hint", spec.id);

        // Normalised <-> plain mapping must agree with the core registry (including log mapping).
        for (const double u : {0.0, 1.0}) {
            const double plain = param->convertFrom0to1(static_cast<float>(u));
            CHECK(near(plain, cd::core::toPlain(spec, u), 1e-3 * (spec.max - spec.min) + 1e-6),
                  "%s endpoint u=%g plain=%g", spec.id, u, plain);
        }
        switch (spec.kind) {
            case ParamKind::Float:
                CHECK(dynamic_cast<juce::AudioParameterFloat*>(param) != nullptr, "%s kind", spec.id);
                for (const double u : {0.1, 0.25, 0.5, 0.75, 0.9}) {
                    const double plain = param->convertFrom0to1(static_cast<float>(u));
                    CHECK(near(plain, cd::core::toPlain(spec, u), 1e-4 * (spec.max - spec.min) + 1e-6),
                          "%s u=%g plain=%g expected=%g", spec.id, u, plain, cd::core::toPlain(spec, u));
                    CHECK(near(param->convertTo0to1(static_cast<float>(plain)), u, 1e-4), "%s inverse u=%g", spec.id, u);
                }
                break;
            case ParamKind::Int:
                CHECK(dynamic_cast<juce::AudioParameterInt*>(param) != nullptr, "%s kind", spec.id);
                break;
            case ParamKind::Bool:
                CHECK(dynamic_cast<juce::AudioParameterBool*>(param) != nullptr, "%s kind", spec.id);
                break;
            case ParamKind::Enum: {
                auto* choice = dynamic_cast<juce::AudioParameterChoice*>(param);
                CHECK(choice != nullptr, "%s kind", spec.id);
                if (choice == nullptr) break;
                CHECK(choice->choices == cd::plugin::enumLabels(spec), "%s labels", spec.id);
                CHECK(choice->choices.size() == static_cast<int>(spec.max - spec.min) + 1, "%s choice count", spec.id);
                break;
            }
        }
    }
    CHECK(seen == cd::plugin::exposedParameterCount(edition), "exposed %zu", seen);
    if (lite) CHECK(seen == 2, "LITE exposes exactly two sound parameters, got %zu", seen);
}

void testBusLayouts() {
    cd::plugin::CircuitDriftProcessor proc(Edition::Full, "CircuitDrift");
    using Layout = juce::AudioProcessor::BusesLayout;
    const auto mono = juce::AudioChannelSet::mono();
    const auto stereo = juce::AudioChannelSet::stereo();
    const auto off = juce::AudioChannelSet::disabled();
    const auto make = [](juce::AudioChannelSet in, juce::AudioChannelSet out, juce::AudioChannelSet sc) {
        Layout l;
        l.inputBuses.add(in);
        l.inputBuses.add(sc);
        l.outputBuses.add(out);
        return l;
    };
    CHECK(proc.isBusesLayoutSupported(make(stereo, stereo, off)), "stereo, no sidechain");
    CHECK(proc.isBusesLayoutSupported(make(stereo, stereo, stereo)), "stereo + stereo sidechain");
    CHECK(proc.isBusesLayoutSupported(make(stereo, stereo, mono)), "stereo + mono sidechain");
    CHECK(proc.isBusesLayoutSupported(make(mono, mono, off)), "mono");
    CHECK(!proc.isBusesLayoutSupported(make(mono, stereo, off)), "mono->stereo rejected");
    CHECK(!proc.isBusesLayoutSupported(make(juce::AudioChannelSet::quadraphonic(),
                                            juce::AudioChannelSet::quadraphonic(), off)),
          "quad rejected");
    CHECK(!proc.isBusesLayoutSupported(Layout{}), "empty bus layout rejected before indexing");
    auto missingSidechain = make(stereo, stereo, off);
    missingSidechain.inputBuses.removeLast();
    CHECK(!proc.isBusesLayoutSupported(missingSidechain), "missing declared sidechain bus rejected");
}

void setPlain(cd::plugin::CircuitDriftProcessor& proc, const char* id, double value) {
    auto* parameter = proc.parameters().getParameter(id);
    CHECK(parameter != nullptr, "parameter %s exists", id);
    if (parameter) parameter->setValueNotifyingHost(parameter->convertTo0to1(static_cast<float>(value)));
}

template <typename Sample>
void testRuntimeAudio() {
    cd::plugin::CircuitDriftProcessor proc(Edition::Full, "CircuitDrift");
    proc.setProcessingPrecision(sizeof(Sample) == 8 ? juce::AudioProcessor::doublePrecision
                                                    : juce::AudioProcessor::singlePrecision);
    setPlain(proc, "mix", 0);
    setPlain(proc, "hpf.enabled", 0);
    setPlain(proc, "output.softClip", 0);
    setPlain(proc, "gainMatch.enabled", 0);
    setPlain(proc, "analogNoise.enabled", 0);
    setPlain(proc, "output.gain", -6);
    proc.prepareToPlay(48000.0, 64);
    const int latency = proc.getLatencySamples();
    CHECK(latency > 0, "runtime declares oversampling latency");
    juce::MidiBuffer midi;
    juce::AudioBuffer<Sample> empty;
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
    proc.processBlock(empty, midi);
    CHECK(midi.isEmpty(), "zero-frame callback accepts an empty audio channel array");
    juce::AudioBuffer<Sample> missingChannel(1, 16);
    for (int i = 0; i < 16; ++i) missingChannel.setSample(0, i, static_cast<Sample>(0.5));
    proc.processBlock(missingChannel, midi);
    CHECK(missingChannel.getMagnitude(0, 16) == 0, "incomplete positive-frame layout is rejected before bus views");
    constexpr int frames = 2048;  // host block deliberately exceeds prepareToPlay's size hint
    juce::AudioBuffer<Sample> buffer(2, frames);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < frames; ++i)
            buffer.setSample(ch, i, static_cast<Sample>(0.1 * std::sin(0.03 * i) * (ch ? -1 : 1)));
    const juce::AudioBuffer<Sample> reference(buffer);
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 63);
    midi.addEvent(juce::MidiMessage::noteOff(1, 60), 127);
    proc.processBlock(buffer, midi);
    CHECK(midi.isEmpty(), "MIDI consumed by effect, not forwarded as output");
    const double gain = std::pow(10.0, -6.0 / 20.0);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < frames; ++i) {
            const double expected = i >= latency ? static_cast<double>(reference.getSample(ch, i - latency)) * gain : 0.0;
            CHECK(near(buffer.getSample(ch, i), expected, 1e-6),
                  "runtime aligned dry with output trim ch%d sample%d got%g expected%g", ch, i,
                  static_cast<double>(buffer.getSample(ch, i)), expected);
        }

    // Bypass still traverses the prepared runtime so host latency stays aligned.
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < frames; ++i) buffer.setSample(ch, i, static_cast<Sample>(0.2));
    proc.processBlockBypassed(buffer, midi);
    CHECK(proc.getLatencySamples() == latency, "bypass preserves reported latency");
    CHECK(near(buffer.getSample(0, frames - 1), 0.2, 1e-6), "host bypass reaches delayed unity dry after crossfade");
    proc.releaseResources();
    proc.processBlock(buffer, midi);
    CHECK(buffer.getMagnitude(0, frames) == 0, "unprepared processor silences main output");
}

template <typename Sample>
void testMonoAuxLayouts() {
    for (const int scChannels : {0, 1, 2}) {
        cd::plugin::CircuitDriftProcessor proc(Edition::Full, "CircuitDrift");
        proc.setProcessingPrecision(sizeof(Sample) == 8 ? juce::AudioProcessor::doublePrecision
                                                       : juce::AudioProcessor::singlePrecision);
        auto layout = proc.getBusesLayout();
        layout.inputBuses.set(0, juce::AudioChannelSet::mono());
        layout.outputBuses.set(0, juce::AudioChannelSet::mono());
        layout.inputBuses.set(1, scChannels == 0 ? juce::AudioChannelSet::disabled()
                                               : scChannels == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo());
        CHECK(proc.setBusesLayout(layout), "mono main with %d sidechain channels", scChannels);
        setPlain(proc, "mix", 0);
        setPlain(proc, "hpf.enabled", 0);
        setPlain(proc, "output.softClip", 0);
        proc.prepareToPlay(48000, 64);
        const int latency = proc.getLatencySamples();
        const int frames = std::max(257, latency + 65);
        juce::AudioBuffer<Sample> buffer(1 + scChannels, frames);
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            for (int i = 0; i < frames; ++i)
                buffer.setSample(ch, i, static_cast<Sample>(ch == 0 ? 0.1 * std::sin(0.03 * i) : 0.17 * ch));
        const juce::AudioBuffer<Sample> reference(buffer);
        juce::MidiBuffer midi;
        proc.processBlock(buffer, midi);
        for (int i = 0; i < frames; ++i) {
            const double expected = i >= latency ? static_cast<double>(reference.getSample(0, i - latency)) : 0.0;
            CHECK(near(buffer.getSample(0, i), expected, 1e-6), "mono main output uses its own channel mapping");
            for (int ch = 1; ch < buffer.getNumChannels(); ++ch)
                CHECK(buffer.getSample(ch, i) == reference.getSample(ch, i), "mono main preserves auxiliary input channels");
        }
    }
}

void testSidechainBufferPreserved() {
    cd::plugin::CircuitDriftProcessor proc(Edition::Full, "CircuitDrift");
    auto layout = proc.getBusesLayout();
    layout.inputBuses.set(1, juce::AudioChannelSet::stereo());
    CHECK(proc.setBusesLayout(layout), "sidechain enabled for adapter test");
    proc.prepareToPlay(48000, 64);
    juce::AudioBuffer<float> buffer(4, 257);
    buffer.clear();
    for (int i = 0; i < buffer.getNumSamples(); ++i) {
        buffer.setSample(0, i, 0.1f); buffer.setSample(1, i, -0.1f);
        buffer.setSample(2, i, 0.37f); buffer.setSample(3, i, -0.23f);
    }
    juce::MidiBuffer midi;
    proc.processBlock(buffer, midi);
    for (int i = 0; i < buffer.getNumSamples(); ++i)
        CHECK(buffer.getSample(2, i) == 0.37f && buffer.getSample(3, i) == -0.23f,
              "sidechain input buffer unchanged at sample%d", i);

    for (int i = 0; i < 1100; ++i)
        midi.addEvent(juce::MidiMessage::noteOn(1, i % 128, static_cast<juce::uint8>(100)), 0);
    proc.processBlock(buffer, midi);
    CHECK(midi.isEmpty() && proc.midiOverflowed(), "oversized MIDI input is bounded and reported");
    proc.processBlock(buffer, midi);
    CHECK(!proc.midiOverflowed(), "MIDI overflow status clears on the next normal block");
}

void testStateRoundTrip() {
    cd::plugin::CircuitDriftProcessor a(Edition::Full, "CircuitDrift");
    cd::plugin::CircuitDriftProcessor b(Edition::Full, "CircuitDrift");
    auto* age = a.parameters().getParameter("macro.age");
    CHECK(age != nullptr, "macro.age exists");
    if (age == nullptr) return;
    age->setValueNotifyingHost(0.7f);
    juce::MemoryBlock blob;
    a.getStateInformation(blob);
    const auto saved = juce::AudioProcessor::getXmlFromBinary(blob.getData(), static_cast<int>(blob.getSize()));
    CHECK(saved && saved->getStringAttribute("edition") == "full" && saved->getStringAttribute("schemaVersion") == "1",
          "new adapter state carries explicit edition and schema");
    b.setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
    CHECK(near(b.parameters().getParameter("macro.age")->getValue(), 0.7, 1e-5), "state round trip");
    const char garbage[] = "not xml";
    b.setStateInformation(garbage, static_cast<int>(sizeof(garbage)));  // must be ignored, not crash
    CHECK(near(b.parameters().getParameter("macro.age")->getValue(), 0.7, 1e-5), "garbage state ignored");
    b.setStateInformation(nullptr, 8);
    b.setStateInformation(garbage, -1);
    b.setStateInformation(garbage, 4 * 1024 * 1024 + 1);  // reject before touching the small buffer
    CHECK(near(b.parameters().getParameter("macro.age")->getValue(), 0.7, 1e-5), "invalid state bounds ignored");

    const auto restoreXml = [&](const juce::XmlElement& xml) {
        juce::MemoryBlock candidate;
        juce::AudioProcessor::copyXmlToBinary(xml, candidate);
        b.setStateInformation(candidate.getData(), static_cast<int>(candidate.getSize()));
    };
    // States produced before adapter identity fields remain readable when their
    // parameter IDs unambiguously belong to this edition.
    const auto legacy = a.parameters().copyState().createXml();
    setPlain(b, "macro.age", 0.1);
    restoreXml(*legacy);
    CHECK(near(b.parameters().getParameter("macro.age")->getValue(), 0.7, 1e-5), "legacy same-edition state remains readable");
    const auto changedCandidate = [&] {
        auto candidate = std::make_unique<juce::XmlElement>(*legacy);
        for (auto* child : candidate->getChildIterator())
            if (child->getStringAttribute("id") == "macro.age") child->setAttribute("value", "0.1");
        return candidate;
    };
    auto invalid = changedCandidate();
    invalid->setAttribute("edition", "lite");
    restoreXml(*invalid);
    CHECK(near(b.parameters().getParameter("macro.age")->getValue(), 0.7, 1e-5), "foreign explicit edition rejected");
    invalid->setAttribute("edition", "full");
    invalid->setAttribute("schemaVersion", "2");
    restoreXml(*invalid);
    CHECK(near(b.parameters().getParameter("macro.age")->getValue(), 0.7, 1e-5), "unsupported adapter schema rejected");
    for (const auto* encoded : {"nan", "inf", "1e999", "0.5 junk", "-0.1", "1.1"}) {
        invalid = std::make_unique<juce::XmlElement>(*legacy);
        for (auto* child : invalid->getChildIterator())
            if (child->getStringAttribute("id") == "macro.age") child->setAttribute("value", encoded);
        restoreXml(*invalid);
        CHECK(near(b.parameters().getParameter("macro.age")->getValue(), 0.7, 1e-5), "invalid parameter encoding rejected: %s", encoded);
    }
    invalid = changedCandidate();
    invalid->getFirstChildElement()->setAttribute("id", "unknown.parameter");
    restoreXml(*invalid);
    CHECK(near(b.parameters().getParameter("macro.age")->getValue(), 0.7, 1e-5), "ambiguous legacy parameters rejected");
    invalid = changedCandidate();
    invalid->addChildElement(new juce::XmlElement(*invalid->getFirstChildElement()));
    restoreXml(*invalid);
    CHECK(near(b.parameters().getParameter("macro.age")->getValue(), 0.7, 1e-5), "duplicate state parameter rejected");

    cd::plugin::CircuitDriftProcessor lite(Edition::Lite, "CircuitDrift LITE");
    setPlain(lite, "lite.sag", 0.6);
    lite.setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
    CHECK(near(lite.parameters().getParameter("lite.sag")->getValue(), 0.6, 1e-5), "full state cannot replace LITE configuration");
    CHECK(!lite.parameters().copyState().getChildWithProperty("id", "macro.age").isValid(), "foreign state children are not installed");
    juce::MemoryBlock liteBlob;
    juce::AudioProcessor::copyXmlToBinary(*legacy, liteBlob);
    lite.setStateInformation(liteBlob.getData(), static_cast<int>(liteBlob.getSize()));
    CHECK(!lite.parameters().copyState().getChildWithProperty("id", "macro.age").isValid(), "legacy full state is rejected by LITE");
    const auto liteLegacy = lite.parameters().copyState().createXml();
    juce::AudioProcessor::copyXmlToBinary(*liteLegacy, liteBlob);
    setPlain(lite, "lite.sag", 0.2);
    lite.setStateInformation(liteBlob.getData(), static_cast<int>(liteBlob.getSize()));
    CHECK(near(lite.parameters().getParameter("lite.sag")->getValue(), 0.6, 1e-5), "legacy LITE state remains readable");
    lite.getStateInformation(liteBlob);
    auto liteXml = juce::AudioProcessor::getXmlFromBinary(liteBlob.getData(), static_cast<int>(liteBlob.getSize()));
    CHECK(liteXml && liteXml->getStringAttribute("edition") == "lite" && liteXml->getStringAttribute("liteMappingVersion") == "1",
          "LITE state identifies its mapping");
    liteXml->setAttribute("liteMappingVersion", "2");
    juce::AudioProcessor::copyXmlToBinary(*liteXml, liteBlob);
    setPlain(lite, "lite.sag", 0.2);
    lite.setStateInformation(liteBlob.getData(), static_cast<int>(liteBlob.getSize()));
    CHECK(near(lite.parameters().getParameter("lite.sag")->getValue(), 0.2, 1e-5), "unsupported LITE mapping rejected");

    // Restore may stage a different latency, but only host reprepare installs it.
    b.prepareToPlay(48000, 64);
    const int originalLatency = b.getLatencySamples();
    setPlain(a, "quality.zeroLatency", 1);
    a.getStateInformation(blob);
    b.setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
    juce::AudioBuffer<float> empty;
    juce::MidiBuffer midi;
    b.processBlock(empty, midi);
    CHECK(b.getLatencySamples() == originalLatency && originalLatency > 0, "restore keeps active latency until reprepare");
    b.prepareToPlay(48000, 64);
    CHECK(b.getLatencySamples() == 0, "reprepare installs restored zero-latency configuration");
}

void testZeroFrameMidi() {
    cd::plugin::CircuitDriftProcessor actual(Edition::Full, "Actual"), reference(Edition::Full, "Reference");
    for (auto* proc : {&actual, &reference}) {
        setPlain(*proc, "key.enabled", 1);
        setPlain(*proc, "key.source", 1);
        setPlain(*proc, "quality.mode", 0);
        setPlain(*proc, "gainMatch.enabled", 0);
        proc->prepareToPlay(48000, 64);
    }
    juce::AudioBuffer<float> empty, a(2, 2048), b(2, 2048);
    juce::MidiBuffer midiA, midiB;
    for (const auto event : {juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
                             juce::MidiMessage::allNotesOff(1)}) {
        midiA.addEvent(event, 0);
        actual.processBlock(empty, midiA);
        midiB.addEvent(event, 0);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 2048; ++i)
                a.setSample(ch, i, static_cast<float>(0.08 * std::sin(0.13 * i)));
        b.makeCopyOf(a);
        actual.processBlock(a, midiA);
        reference.processBlock(b, midiB);
        double difference = 0;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 2048; ++i)
                difference = std::max(difference, std::fabs(double(a.getSample(ch, i)) - b.getSample(ch, i)));
        CHECK(difference < 1e-7, "zero-frame MIDI flush matches sample-zero event in next audio block: %g", difference);
    }
}

}  // namespace

int main() {
    juce::ScopedJuceInitialiser_GUI gui;
    testGoldenTable();
    testEdition(Edition::Full);
    testEdition(Edition::Lite);
    testBusLayouts();
    testRuntimeAudio<float>();
    testRuntimeAudio<double>();
    testMonoAuxLayouts<float>();
    testMonoAuxLayouts<double>();
    testSidechainBufferPreserved();
    testStateRoundTrip();
    testZeroFrameMidi();
    g_failures += cd::plugin::test::processor_regressions::run();
    g_failures += cd::plugin::test::gesture_regressions::run();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("plugin parameter consistency: all checks passed (%zu registry params)\n", cd::core::kParamCount);
    return 0;
}
