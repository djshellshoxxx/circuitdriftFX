// Host restore coherence using the production APVTS, adapter and runtime. The test
// pauses a real parameter notification; it does not add a production scheduling hook.
#pragma once
#include "PluginProcessor.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <future>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace cd::plugin::test::host_restore_regressions {

// Count C++ new/new[] calls on the simulated audio owner only. The publisher and
// pitch worker may allocate off audio. This does not instrument direct C malloc.
inline thread_local bool countAllocations = false;
inline thread_local unsigned allocations = 0;

struct Observation {
    double mainsA = 0, mainsB = 0, sag = 0, age = 0;
    bool bypassed = false;
    std::uint64_t generation = 0;
};

class PausedRestore final : private juce::AudioProcessorValueTreeState::Listener {
public:
    PausedRestore(CircuitDriftProcessor& processor, const juce::String& firstId,
                  const juce::MemoryBlock& candidate)
        : processor_(processor), firstId_(firstId) {
        processor_.parameters().addParameterListener(firstId_, this);
        try {
            writer_ = std::thread([this, &candidate] {
                try {
                    processor_.setStateInformation(candidate.getData(), static_cast<int>(candidate.getSize()));
                } catch (...) { failure_ = std::current_exception(); }
            });
        } catch (...) {
            processor_.parameters().removeParameterListener(firstId_, this);
            throw;
        }
    }
    ~PausedRestore() override {
        finish();
        processor_.parameters().removeParameterListener(firstId_, this);
    }
    bool waitUntilPaused() {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(5), [&] { return entered_; });
    }
    void finish() {
        {
            const std::lock_guard lock(mutex_);
            released_ = true;
        }
        condition_.notify_all();
        if (writer_.joinable()) writer_.join();
    }
    bool completedNormally() const noexcept { return !timedOut_ && !failure_; }
private:
    void parameterChanged(const juce::String& id, float) override {
        if (id != firstId_) return;
        std::unique_lock lock(mutex_);
        if (entered_) return;
        entered_ = true;
        condition_.notify_all();
        // A broken implementation that waits on the publisher from audio must
        // fail the fixture instead of leaving the native executable hung forever.
        timedOut_ = !condition_.wait_for(lock, std::chrono::seconds(10), [&] { return released_; });
    }
    CircuitDriftProcessor& processor_;
    juce::String firstId_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_ = false, released_ = false, timedOut_ = false;
    std::exception_ptr failure_;
    std::thread writer_;
};

// Exercise host callbacks that reenter save/restore or abort one notification.
// The first notification is one-shot; no production hook or allocator failure is needed.
class NotificationProbe final : private juce::AudioProcessorValueTreeState::Listener {
public:
    NotificationProbe(CircuitDriftProcessor& processor, const juce::String& id,
                      const juce::MemoryBlock* nestedRestore, bool fail)
        : processor_(processor), id_(id), nestedRestore_(nestedRestore), fail_(fail) {
        processor_.parameters().addParameterListener(id_, this);
    }
    ~NotificationProbe() override { processor_.parameters().removeParameterListener(id_, this); }
    bool called = false;
    juce::MemoryBlock savedDuringNotification;
private:
    void parameterChanged(const juce::String&, float) override {
        if (called) return;
        called = true;
        processor_.getStateInformation(savedDuringNotification);
        if (nestedRestore_)
            processor_.setStateInformation(nestedRestore_->getData(), static_cast<int>(nestedRestore_->getSize()));
        if (fail_) throw std::runtime_error("synthetic host notification failure");
    }
    CircuitDriftProcessor& processor_;
    juce::String id_;
    const juce::MemoryBlock* nestedRestore_;
    bool fail_;
};

inline int run() {
    int failures = 0;
    const auto check = [&](bool value, const char* message) {
        if (!value) { ++failures; std::fprintf(stderr, "FAIL host restore: %s\n", message); }
    };
    const auto near = [](double a, double b) { return std::abs(a - b) < 1e-4; };
    const auto set = [&](CircuitDriftProcessor& processor, const char* id, float plain) {
        auto* parameter = processor.parameters().getParameter(id);
        check(parameter != nullptr, "test parameter exists");
        if (parameter) parameter->setValueNotifyingHost(parameter->convertTo0to1(plain));
    };
    const auto candidate = [&](CircuitDriftProcessor& processor,
                               std::initializer_list<std::pair<const char*, float>> values) {
        juce::MemoryBlock current, result;
        processor.getStateInformation(current);
        auto xml = juce::AudioProcessor::getXmlFromBinary(current.getData(), static_cast<int>(current.getSize()));
        check(xml != nullptr, "source state decodes");
        if (!xml) return result;
        // Valid legacy-compatible subsets retain omitted parameters. Explicit
        // child ordering establishes which changed parameter reaches JUCE first.
        xml->deleteAllChildElements();
        for (const auto& [id, value] : values) {
            auto* parameter = new juce::XmlElement("PARAM");
            parameter->setAttribute("id", id);
            parameter->setAttribute("value", static_cast<double>(value));
            xml->addChildElement(parameter);
        }
        juce::AudioProcessor::copyXmlToBinary(*xml, result);
        return result;
    };
    const auto restore = [](CircuitDriftProcessor& processor, const juce::MemoryBlock& state) {
        processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    };
    const auto encodedValue = [&](const juce::MemoryBlock& state, const char* id, double expected) {
        const auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
        bool found = false;
        if (xml) for (const auto* child : xml->getChildIterator()) {
            if (child->getStringAttribute("id") == id) {
                found = near(child->getDoubleAttribute("value"), expected);
                break;
            }
        }
        return found;
    };
    const auto savedValue = [&](CircuitDriftProcessor& processor, const char* id, double expected) {
        juce::MemoryBlock state;
        processor.getStateInformation(state);
        return encodedValue(state, id, expected);
    };
    auto snapshot = std::make_unique<CircuitDriftProcessor::EditorSnapshot>();
    juce::AudioBuffer<float> empty, block(2, 16);
    juce::MidiBuffer midi;
    const auto observe = [&](CircuitDriftProcessor& processor, bool positiveFrames = false) {
        block.clear();
        allocations = 0;
        countAllocations = true;
        processor.processBlock(positiveFrames ? block : empty, midi);
        countAllocations = false;
        check(allocations == 0, "restoring/accepted callback makes no C++ heap allocation");
        check(processor.readEditorSnapshot(*snapshot), "callback publishes runtime targets");
        return Observation{snapshot->targets[0].mainsVolts, snapshot->targets[1].mainsVolts,
                           snapshot->targets[0].sagAugmentation, snapshot->targets[0].ageYears,
                           snapshot->telemetry.bypassed, snapshot->generation};
    };

    for (const auto edition : {Edition::Full, Edition::Lite}) {
        const bool lite = edition == Edition::Lite;
        CircuitDriftProcessor processor(edition, "Coherent restore");
        const char* firstId = lite ? kBypassParamId : "slot.a.supply.mainsVoltage";
        const char* secondId = lite ? "lite.sag" : "slot.b.supply.mainsVoltage";
        if (lite) {
            set(processor, "lite.sag", 0.1f);
            set(processor, "lite.thermalAge", 0.2f);
        } else {
            set(processor, "slot.a.supply.mainsVoltage", 111);
            set(processor, "slot.b.supply.mainsVoltage", 119);
        }
        const auto oldState = lite
            ? candidate(processor, {{kBypassParamId, 0}, {"lite.sag", 0.1f}, {"lite.thermalAge", 0.2f}})
            : candidate(processor, {{"slot.a.supply.mainsVoltage", 111}, {"slot.b.supply.mainsVoltage", 119}});
        const auto newState = lite
            ? candidate(processor, {{kBypassParamId, 1}, {"lite.sag", 0.8f}, {"lite.thermalAge", 0.6f}})
            : candidate(processor, {{"slot.a.supply.mainsVoltage", 95}, {"slot.b.supply.mainsVoltage", 125}});
        const auto* firstRaw = processor.parameters().getRawParameterValue(firstId);
        const auto* secondRaw = processor.parameters().getRawParameterValue(secondId);
        check(firstRaw && secondRaw, "both paused-restore parameters exist");
        if (!firstRaw || !secondRaw) continue;
        processor.prepareToPlay(48000, 64);
        const auto before = observe(processor);
        const auto isOld = [&](const Observation& value) {
            return near(value.mainsA, before.mainsA) && near(value.mainsB, before.mainsB) &&
                   near(value.sag, before.sag) && near(value.age, before.age) && value.bypassed == before.bypassed;
        };
        const auto isNew = [&](const Observation& value) {
            return lite ? near(value.sag, 2.4) && near(value.age, 36) && value.bypassed
                        : near(value.mainsA, 95) && near(value.mainsB, 125);
        };
        {
            PausedRestore publisher(processor, firstId, newState);
            const bool paused = publisher.waitUntilPaused();
            check(paused, "valid restore reaches the first changed APVTS parameter");
            if (paused) {
                check(near(firstRaw->load(), lite ? 1 : 95) && near(secondRaw->load(), lite ? 0.1 : 119),
                      "fixture observes a real partially replaced APVTS state");
                check(isOld(observe(processor)), "zero-frame callback keeps the complete previous configuration during restore");
                check(isOld(observe(processor, true)), "positive-frame callback keeps the complete previous configuration during restore");
            }
            publisher.finish();
            check(publisher.completedNormally(), "publisher completes without timeout or exception; audio never waits for it");
        }
        check(isNew(observe(processor)), "next zero-frame boundary adopts the complete restored configuration");
        check(savedValue(processor, firstId, lite ? 1 : 95) && savedValue(processor, secondId, lite ? 0.8 : 125),
              "accepted restore is saved coherently without waiting for another audio callback");

        if (lite) {
            set(processor, kBypassParamId, 0);
            set(processor, "lite.sag", 0.3f);
            set(processor, "lite.thermalAge", 0.4f);
            const auto automated = observe(processor);
            check(near(automated.sag, 0.9) && near(automated.age, 24) && !automated.bypassed,
                  "LITE automation including bypass resumes after restore adoption");
        } else {
            set(processor, "slot.a.supply.mainsVoltage", 103);
            set(processor, "slot.b.supply.mainsVoltage", 121);
            const auto automated = observe(processor);
            check(near(automated.mainsA, 103) && near(automated.mainsB, 121),
                  "ordinary host automation resumes after restore adoption");
        }

        // Two accepted restores and a rejected candidate can all occur while
        // audio is stopped. The most recent complete host state must be saved.
        restore(processor, oldState);
        auto invalidXml = juce::AudioProcessor::getXmlFromBinary(newState.getData(), static_cast<int>(newState.getSize()));
        juce::MemoryBlock invalid;
        check(invalidXml != nullptr, "invalid-restore fixture starts from real XML");
        if (invalidXml) {
            invalidXml->setAttribute("schemaVersion", "unsupported");
            juce::AudioProcessor::copyXmlToBinary(*invalidXml, invalid);
            restore(processor, invalid);
        }
        check(savedValue(processor, firstId, lite ? 0 : 111) && savedValue(processor, secondId, lite ? 0.1 : 119),
              "invalid restore between valid states changes no saved parameter");
        restore(processor, newState);
        check(savedValue(processor, firstId, lite ? 1 : 95) && savedValue(processor, secondId, lite ? 0.8 : 125),
              "immediate stopped-audio save retains the latest accepted restore");
        processor.releaseResources();
        processor.prepareToPlay(44100, 64);
        const auto reprepared = observe(processor);
        check(isNew(reprepared) && reprepared.generation != before.generation,
              "reprepare uses the latest complete host state without an old pending generation");

        // Both threads enter the host API while the first publication is paused.
        // Source synchronization serializes publishers; audio still never waits.
        {
            PausedRestore first(processor, firstId, oldState);
            const bool paused = first.waitUntilPaused();
            check(paused, "first of two concurrent publishers reaches its notification");
            std::promise<void> secondEntered;
            auto secondReady = secondEntered.get_future();
            std::exception_ptr secondFailure;
            std::thread second([&] {
                secondEntered.set_value();
                try { restore(processor, newState); }
                catch (...) { secondFailure = std::current_exception(); }
            });
            check(secondReady.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
                  "second host publisher starts within the bounded setup window");
            if (paused) check(isNew(observe(processor)), "overlapping host publishers keep the previous complete audio configuration");
            first.finish();
            second.join();
            check(first.completedNormally() && !secondFailure, "both concurrent host publications complete normally");
        }
        check(isNew(observe(processor)), "latest serialized host publisher supplies a complete configuration");
        {
            NotificationProbe probe(processor, firstId, &newState, false);
            restore(processor, oldState);
            check(probe.called && encodedValue(probe.savedDuringNotification, firstId, lite ? 1 : 95) &&
                  encodedValue(probe.savedDuringNotification, secondId, lite ? 0.8 : 125),
                  "reentrant save sees the complete preceding host state while replacement is active");
            check(isOld(observe(processor)), "nested restore cannot invalidate its outer APVTS traversal");
        }
        {
            NotificationProbe probe(processor, firstId, nullptr, true);
            bool threw = false;
            try { restore(processor, newState); }
            catch (const std::runtime_error&) { threw = true; }
            check(threw && probe.called, "controlled notification failure aborts a real partial replacement");
            check(isOld(observe(processor)) && savedValue(processor, firstId, lite ? 0 : 111) &&
                  savedValue(processor, secondId, lite ? 0.1 : 119),
                  "failed replacement leaves audio and host saves on the complete preceding configuration");
        }
        processor.releaseResources();
        processor.prepareToPlay(48000, 64);
        check(isOld(observe(processor)), "reprepare after a failed notification uses the complete fallback");
        const auto recovery = candidate(processor, {{secondId, lite ? 0.8f : 125.0f}});
        restore(processor, recovery);
        const auto recovered = observe(processor);
        check(lite ? near(recovered.sag, 2.4) && near(recovered.age, 12) && !recovered.bypassed
                   : near(recovered.mainsA, 111) && near(recovered.mainsB, 125),
              "successful legacy-subset recovery retains omitted fallback values and reopens audio reads");
        set(processor, firstId, lite ? 1.0f : 103.0f);
        const auto afterRecoveryAutomation = observe(processor);
        check(lite ? afterRecoveryAutomation.bypassed : near(afterRecoveryAutomation.mainsA, 103),
              "ordinary automation resumes after exceptional restore recovery");

        // A successful stopped-audio restore must not be lost when a later
        // notification fails before audio has adopted that successful state.
        restore(processor, oldState);  // A
        check(isOld(observe(processor)), "three-state fixture starts with audio on A");
        restore(processor, newState);  // B: deliberately no callback before C.
        const auto thirdState = lite
            ? candidate(processor, {{kBypassParamId, 0}, {"lite.sag", 0.3f}, {"lite.thermalAge", 0.4f}})
            : candidate(processor, {{"slot.a.supply.mainsVoltage", 103}, {"slot.b.supply.mainsVoltage", 121}});
        {
            NotificationProbe probe(processor, firstId, nullptr, true);
            bool threw = false;
            try { restore(processor, thirdState); }  // C
            catch (const std::runtime_error&) { threw = true; }
            check(threw && probe.called && near(firstRaw->load(), lite ? 0 : 103),
                  "third-state notification fails after publishing its first changed parameter");
        }
        check(savedValue(processor, firstId, lite ? 1 : 95) && savedValue(processor, secondId, lite ? 0.8 : 125),
              "failed C restore preserves successful stopped-audio B in the saved state");
        check(isNew(observe(processor)), "first resumed zero-frame callback adopts B after failed C");
        check(isNew(observe(processor, true)), "resumed positive-frame callback retains B after failed C");
        restore(processor, newState);
        check(isNew(observe(processor)), "three-state failure recovers through a successful restore");

        CircuitDriftProcessor initiallyStopped(edition, "Unprepared restore");
        restore(initiallyStopped, newState);
        check(savedValue(initiallyStopped, firstId, lite ? 1 : 95) && savedValue(initiallyStopped, secondId, lite ? 0.8 : 125),
              "restore before first prepare is immediately saveable");
        initiallyStopped.prepareToPlay(48000, 64);
        check(isNew(observe(initiallyStopped)), "first preparation adopts the complete preloaded host configuration");
    }
    std::printf("Host restore regressions: %s (%d failures)\n", failures ? "FAILED" : "PASS", failures);
    return failures;
}

}  // namespace cd::plugin::test::host_restore_regressions
