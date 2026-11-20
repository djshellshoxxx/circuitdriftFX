// Real adapter state/lifecycle tests; not native-host or realtime deadline qualification.
#pragma once
#include "PluginProcessor.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <future>
#include <memory>
#include <string>
#include <thread>

namespace cd::plugin::test::processor_regressions {

inline bool sameSettings(const UiSettings& a, const UiSettings& b) {
    return a.scale == b.scale && a.darkTheme == b.darkTheme && a.view == b.view &&
           a.educational == b.educational && a.reduceMotion == b.reduceMotion &&
           a.lowFrameRate == b.lowFrameRate && a.overlay == b.overlay && a.xray == b.xray &&
           a.pcbView == b.pcbView && a.lastWidth == b.lastWidth && a.lastHeight == b.lastHeight;
}

struct StateListener final : juce::AudioProcessorListener {
    int nonParameterChanges = 0;
    void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override {}
    void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails& details) override {
        if (details.nonParameterStateChanged) ++nonParameterChanges;
    }
};

inline int run() {
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) { ++failures; std::fprintf(stderr, "FAIL processor: %s\n", message); }
    };
    const auto set = [&](CircuitDriftProcessor& processor, const char* id, float plain) {
        auto* parameter = processor.parameters().getParameter(id);
        check(parameter != nullptr, "regression parameter exists");
        if (parameter) parameter->setValueNotifyingHost(parameter->convertTo0to1(plain));
    };

    for (const auto edition : {Edition::Full, Edition::Lite}) {
        CircuitDriftProcessor source(edition, "Source"), restored(edition, "Restored");
        StateListener sourceListener, restoreListener;
        source.addListener(&sourceListener);
        restored.addListener(&restoreListener);
        UiSettings settings;
        settings.scale = 1.75f;
        settings.darkTheme = false;
        settings.view = EditorView::Workbench;
        settings.educational = settings.reduceMotion = settings.lowFrameRate = true;
        settings.overlay = 3;
        settings.xray = true;
        settings.pcbView = 2;
        settings.lastWidth = 1370;
        settings.lastHeight = 910;
        source.setEditorSettings(settings);
        check(sourceListener.nonParameterChanges == 1, "changed editor preferences mark host state dirty");
        source.setEditorSettings(settings);
        check(sourceListener.nonParameterChanges == 1, "identical editor preferences do not dirty host state again");
        const char* id = edition == Edition::Full ? "macro.age" : "lite.thermalAge";
        set(source, id, 0.6f);
        juce::MemoryBlock state;
        source.getStateInformation(state);
        restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        check(sameSettings(restored.editorSettings(), settings), "all editor preferences survive Full/LITE host reload");
        check(restoreListener.nonParameterChanges == 0, "host restore does not mark its own restored state dirty");
        auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
        check(xml && xml->getStringAttribute("uiVersion") == "1", "editor preferences have an explicit format version");
        if (xml) {
            for (const auto* attribute : {"uiVersion", "uiScale", "uiView", "uiDarkTheme", "uiWidth"}) {
                juce::XmlElement changed(*xml);
                changed.setAttribute(attribute, "9999");
                for (auto* child : changed.getChildIterator())
                    if (child->getStringAttribute("id") == id) child->setAttribute("value", "0.2");
                juce::MemoryBlock invalid;
                juce::AudioProcessor::copyXmlToBinary(changed, invalid);
                restored.setStateInformation(invalid.getData(), static_cast<int>(invalid.getSize()));
                check(sameSettings(restored.editorSettings(), settings), "invalid preferences preserve previous preferences");
                check(std::abs(restored.parameters().getRawParameterValue(id)->load() - 0.6f) < 1e-6f,
                      "invalid preferences reject the complete parameter transaction");
            }
            juce::XmlElement missing(*xml);
            missing.removeAttribute("uiHeight");
            juce::MemoryBlock incomplete;
            juce::AudioProcessor::copyXmlToBinary(missing, incomplete);
            restored.setStateInformation(incomplete.getData(), static_cast<int>(incomplete.getSize()));
            check(sameSettings(restored.editorSettings(), settings), "incomplete versioned preferences are rejected");
        }
        // Source's APVTS has never loaded serialized preference attributes.
        const auto legacy = source.parameters().copyState().createXml();
        check(legacy && !legacy->hasAttribute("uiVersion"), "legacy fixture is genuinely parameter-only");
        if (legacy) {
            juce::AudioProcessor::copyXmlToBinary(*legacy, state);
            restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
            check(sameSettings(restored.editorSettings(), UiSettings{}), "legacy host state selects default editor preferences");
            check(std::abs(restored.parameters().getRawParameterValue(id)->load() - 0.6f) < 1e-6f,
                  "legacy host state retains its sound parameters");
        }
        source.removeListener(&sourceListener);
        restored.removeListener(&restoreListener);
    }

    {
        CircuitDriftProcessor processor(Edition::Full, "Snapshots");
        auto snapshot = std::make_unique<CircuitDriftProcessor::EditorSnapshot>();
        snapshot->bpm = 77;
        check(!processor.readEditorSnapshot(*snapshot) && snapshot->bpm == 77,
              "failed snapshot reads preserve caller output");
        check(!processor.requestEngineReset(false), "unprepared processor rejects reset requests");
        set(processor, "tempo.enabled", 1);
        set(processor, "key.enabled", 1);
        set(processor, "key.source", 1);
        processor.prepareToPlay(48000, 64);
        check(!processor.readEditorSnapshot(*snapshot), "preparation does not invent a snapshot");
        juce::AudioBuffer<float> buffer(2, 64), empty;
        buffer.clear();
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
        processor.processBlock(buffer, midi);
        check(processor.readEditorSnapshot(*snapshot), "audio publishes a complete editor snapshot");
        check(snapshot->telemetry.sampleRate == 48000 && snapshot->targets[0].mainsVolts > 0,
              "snapshot contains real sample rate and effective targets");
        check(snapshot->controls.tempoFallback && snapshot->bpm == 120,
              "missing host tempo has an explicit published fallback");
        check(snapshot->controls.keyState == control::KeyState::NoteActive,
              "published control state contains the held MIDI note");
        const auto generation = snapshot->generation;
        const auto sampleIndex = snapshot->telemetry.sampleIndex;
        check(processor.readEditorSnapshot(*snapshot) && snapshot->generation == generation &&
              snapshot->telemetry.sampleIndex == sampleIndex,
              "serial UI consumers can reuse the latest snapshot without advancing it");
        set(processor, "quality.zeroLatency", 1);
        processor.processBlock(empty, midi);
        check(processor.readEditorSnapshot(*snapshot) && snapshot->telemetry.reprepareRequired &&
              !snapshot->telemetry.activeZeroLatency && snapshot->telemetry.pendingZeroLatency &&
              snapshot->telemetry.sampleIndex == sampleIndex,
              "zero-frame callback publishes pending quality without advancing the sample clock");
        check(processor.requestEngineReset(false), "prepared reset request accepted");
        processor.processBlock(empty, midi);
        check(processor.readEditorSnapshot(*snapshot) && snapshot->controls.keyState == control::KeyState::ManualReference &&
              !snapshot->telemetry.needsReset && !snapshot->telemetry.solverFallback &&
              snapshot->telemetry.sampleIndex == sampleIndex,
              "zero-frame reset publishes cleared control/reset state without advancing time");
        processor.releaseResources();
        check(!processor.readEditorSnapshot(*snapshot), "released runtime has no readable snapshot");
        processor.prepareToPlay(44100, 64);
        check(!processor.readEditorSnapshot(*snapshot), "old sample-rate snapshot cannot cross a preparation generation");
        processor.processBlock(buffer, midi);
        check(processor.readEditorSnapshot(*snapshot) && snapshot->telemetry.sampleRate == 44100 &&
              snapshot->telemetry.activeZeroLatency && snapshot->generation != generation,
              "reprepared generation publishes its actual configuration");
    }
    for (const bool cold : {false, true}) {
        CircuitDriftProcessor actual(Edition::Full, "Reset adapter");
        actual.setProcessingPrecision(juce::AudioProcessor::doublePrecision);
        set(actual, "macro.drift", 0.7f);
        set(actual, "analogNoise.enabled", 1);
        actual.prepareToPlay(48000, 64);
        auto parameters = core::defaultValues();
        for (std::size_t i = 0; i < parameters.size(); ++i)
            if (auto* raw = actual.parameters().getRawParameterValue(core::kParams[i].id)) parameters[i] = raw->load();
        engine::EngineConfig config;
        config.maxBlock = 64;
        runtime::Processor reference;
        std::string error;
        if (!reference.prepare(config, parameters, false, error)) {
            check(false, "quiescent reset reference prepares");
            continue;
        }
        juce::AudioBuffer<double> a(2, 128), b(2, 128), empty;
        juce::MidiBuffer midi;
        engine::ProcessContext context;
        control::ControlContext controls;
        const auto fill = [](juce::AudioBuffer<double>& buffer) {
            for (int channel = 0; channel < 2; ++channel)
                for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                    buffer.setSample(channel, sample, 0.08 * std::sin(0.13 * sample));
        };
        const auto processReference = [&](int frames) {
            const double* input[2] = {b.getReadPointer(0), b.getReadPointer(1)};
            double* output[2] = {b.getWritePointer(0), b.getWritePointer(1)};
            controls.frames = frames;
            controls.sampleStart = static_cast<std::uint64_t>(context.sampleStart);
            context.frames = static_cast<std::uint32_t>(frames);
            reference.process(context, controls, input, output, 2, nullptr, 0, frames, parameters);
            context.sampleStart += frames;
        };
        // Both paths see identical targets and history. A freshly prepared
        // reference could have different modulation targets at reset time.
        for (int block = 0; block < 4; ++block) {
            fill(a);
            fill(b);
            actual.processBlock(a, midi);
            processReference(128);
        }
        check(actual.requestEngineReset(!cold) && actual.requestEngineReset(cold),
              "latest reset request replaces the earlier one");
        reference.reset(cold ? engine::ResetKind::Cold : engine::ResetKind::Warm);
        actual.processBlock(empty, midi);
        processReference(0);
        fill(a);
        fill(b);
        actual.processBlock(a, midi);
        processReference(128);
        double difference = 0;
        for (int channel = 0; channel < 2; ++channel)
            for (int sample = 0; sample < 128; ++sample)
                difference = std::max(difference, std::abs(a.getSample(channel, sample) - b.getSample(channel, sample)));
        check(difference < 1e-10, "queued Warm/Cold reset at zero frames matches the quiescent runtime reference");
    }
    {
        CircuitDriftProcessor processor(Edition::Full, "Lifecycle reads");
        juce::AudioBuffer<float> buffer(2, 64);
        juce::MidiBuffer midi;
        processor.prepareToPlay(48000, 64);
        buffer.clear();
        processor.processBlock(buffer, midi);
        std::atomic<bool> done{false}, coherent{true};
        std::atomic<unsigned> accepted{0};
        std::promise<void> started;
        auto ready = started.get_future();
        std::thread reader([&] {
            auto snapshot = std::make_unique<CircuitDriftProcessor::EditorSnapshot>();
            const auto read = [&] {
                if (processor.readEditorSnapshot(*snapshot)) {
                    accepted.fetch_add(1, std::memory_order_relaxed);
                    if (snapshot->telemetry.sampleRate != 44100 && snapshot->telemetry.sampleRate != 48000)
                        coherent.store(false, std::memory_order_relaxed);
                }
            };
            read();
            started.set_value();
            while (!done.load(std::memory_order_acquire)) {
                read();
                std::this_thread::yield();
            }
        });
        check(ready.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
              "snapshot reader starts within the bounded test setup window");
        for (int iteration = 0; iteration < 4; ++iteration) {
            processor.prepareToPlay(iteration % 2 ? 44100 : 48000, 64);
            buffer.clear();
            processor.processBlock(buffer, midi);
            processor.releaseResources();
        }
        done.store(true, std::memory_order_release);
        reader.join();
        check(coherent.load() && accepted.load() > 0,
              "single UI reader remains coherent across sequential host prepare/audio/release generations");
    }
    return failures;
}
}  // namespace cd::plugin::test::processor_regressions
