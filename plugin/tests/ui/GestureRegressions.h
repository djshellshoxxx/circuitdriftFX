// Host-visible gesture regressions. Uses real JUCE parameters, synthetic bridge only.
#pragma once
#include "FakeBridge.h"
#include "ui/panels/PanelCommon.h"
#include "ui/workbench/ParamOps.h"

#include <cstdio>
#include <limits>

namespace cd::plugin::test::gesture_regressions {
inline int failures = 0;
inline void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", what); }
}
class Bridge final : public cd::plugin::test::FakeBridge {
public:
    void beginGesture(const juce::String&) override { ++begins; ++open; }
    void endGesture() override { ++ends; --open; }
    int begins = 0, ends = 0, open = 0;
};
struct HostGestures final : juce::AudioProcessorParameter::Listener {
    explicit HostGestures(juce::RangedAudioParameter& p) : parameter(p) { parameter.addListener(this); }
    ~HostGestures() override { parameter.removeListener(this); }
    void parameterValueChanged(int, float) override { ++writes; }
    void parameterGestureChanged(int, bool starting) override { starting ? ++begins : ++ends; }
    juce::RangedAudioParameter& parameter;
    int begins = 0, ends = 0, writes = 0;
};

inline cd::plugin::ui::pc::ParamSlider* findSlider(juce::Component& root, const juce::String& idOrTitle) {
    if (auto* slider = dynamic_cast<cd::plugin::ui::pc::ParamSlider*>(&root))
        if (slider->binding.id() == idOrTitle || slider->slider.getTitle() == idOrTitle) return slider;
    for (auto* child : root.getChildren())
        if (auto* result = findSlider(*child, idOrTitle)) return result;
    return nullptr;
}

inline void numericPanelRegressions(Bridge& bridge) {
    using namespace cd::plugin::ui::pc;
    auto* crossfade = bridge.parameters().getParameter("ab.crossfade");
    crossfade->setValueNotifyingHost(0.373f);
    auto ab = makeABPanel(bridge);
    auto* cross = findSlider(*ab, "ab.crossfade");
    check(cross != nullptr, "production A/B slider found");
    if (cross) {
        auto& slider = cross->slider;
        const double before = slider.getValue();
        check(slider.valueFromTextFunction(slider.getTextFromValue(before)) == before,
              "unchanged rounded A/B text preserves exact value");
        for (const auto* bad : {"1 0", "1e 2", "garbage", "1e999", "+-1", "0.5 junk", "50% A / 60% B"})
            check(slider.valueFromTextFunction(bad) == before, "invalid A/B text does not edit the parameter");
        slider.setValue(slider.valueFromTextFunction("25% A / 75% B"), juce::sendNotificationSync);
        check(std::abs(crossfade->getValue() - 0.75f) < 1e-6, "A/B displayed percentages edit the B fraction");
    }
    auto* harmonic = bridge.parameters().getParameter("harmonics.target");
    harmonic->setValueNotifyingHost(harmonic->convertTo0to1(-0.373f));
    auto musical = makeMusicalPanel(bridge);
    auto* target = findSlider(*musical, "harmonics.target");
    check(target != nullptr, "production harmonic slider found");
    if (target) {
        auto& slider = target->slider;
        const double before = slider.getValue();
        check(slider.valueFromTextFunction(slider.getTextFromValue(before)) == before,
              "unchanged rounded harmonic text preserves exact value");
        slider.setValue(slider.valueFromTextFunction("Odd 50 %"), juce::sendNotificationSync);
        check(std::abs(harmonic->convertFrom0to1(harmonic->getValue()) + 0.5f) < 1e-6,
              "Odd harmonic text preserves its negative sign");
        slider.setValue(slider.valueFromTextFunction("Even 50 %"), juce::sendNotificationSync);
        check(std::abs(harmonic->convertFrom0to1(harmonic->getValue()) - 0.5f) < 1e-6,
              "Even harmonic text preserves its positive sign");
    }
    auto faults = makeFaultsPanel(bridge);
    auto* frequency = findSlider(*faults, "Resonance frequency");
    check(frequency != nullptr, "production fault frequency slider found");
    if (frequency) {
        auto& slider = frequency->slider;
        check(slider.valueFromTextFunction("8e2 Hz") == 800.0, "fault frequency accepts scientific notation");
        check(slider.valueFromTextFunction("0.8 kHz") == 800.0, "fault frequency accepts engineering units");
        for (const auto* bad : {"1 0", "8e 2 Hz", "garbage", "800 V", "800 Hz junk", "4001 Hz"})
            check(slider.valueFromTextFunction(bad) == slider.getValue(), "invalid fault text preserves its value");
    }
    check(bridge.open == 0 && bridge.begins == bridge.ends, "numeric panel gestures close");
}

inline int run() {
    Bridge bridge;
    auto& a = *bridge.parameters().getParameter("slot.a.stage.bias");
    auto& b = *bridge.parameters().getParameter("slot.b.stage.bias");
    auto& drive = *bridge.parameters().getParameter("slot.a.stage.drive");
    a.setValueNotifyingHost(0.2f);
    b.setValueNotifyingHost(0.8f);
    HostGestures ha(a), hb(b), hd(drive);

    {
        cd::plugin::ui::ParamOps ops(bridge);
        check(!ops.begin("empty", {}), "empty transaction rejected");
        check(ops.begin("Bias", {"stage.bias", "stage.bias"}), "duplicate parameter transaction opens once");
        check(ha.begins == 1, "one host begin for repeated parameter");
        bridge.setEditSlot(1);
        ops.setNormalized("stage.bias", 0.6);
        check(std::abs(a.getValue() - 0.6f) < 1e-6 && std::abs(b.getValue() - 0.8f) < 1e-6,
              "changing edit slot does not redirect a live gesture");
        const float untouchedDrive = drive.getValue();
        ops.setNormalized("stage.drive", 0.1);
        check(drive.getValue() == untouchedDrive && hd.writes == 0, "unlisted parameter cannot join active transaction");
        ops.setNormalized("stage.bias", std::numeric_limits<double>::quiet_NaN());
        ops.setReal("stage.bias", std::numeric_limits<double>::infinity());
        check(std::abs(a.getValue() - 0.6f) < 1e-6, "non-finite edits do not change parameters");
        ops.rollback();
        check(std::abs(a.getValue() - 0.2f) < 1e-6 && std::abs(b.getValue() - 0.8f) < 1e-6,
              "rollback restores the original slot after slot selection changes");
        check(ha.begins == 1 && ha.ends == 1 && hb.begins == 0 && hb.ends == 0,
              "host changes end on the parameter that began them");
    }
    check(bridge.open == 0 && bridge.begins == bridge.ends, "workbench transaction closed");

    {
        cd::plugin::ui::pc::ParamBinding binding(bridge, "Bias");
        binding.bind("slot.a.stage.bias", [](float) {});
        binding.beginPart();
        binding.setPart(0.3f);
        binding.bind("slot.b.stage.bias", [](float) {});
        check(ha.begins == 2 && ha.ends == 2 && bridge.open == 0,
              "rebind closes active host and undo gestures on old parameter");
        const float before = b.getValue();
        binding.setPart(0.4f);
        check(b.getValue() == before && hb.begins == 0, "continuing old drag cannot edit new binding");
        binding.beginPart();
        binding.setComplete(0.2f);
        check(hb.begins == 1 && hb.ends == 0 && bridge.open == 1,
              "complete edit during a live gesture does not nest host gestures");
    }
    check(hb.begins == 1 && hb.ends == 1 && bridge.open == 0 && bridge.begins == bridge.ends,
          "destroying binding closes its active host and undo gestures");
    numericPanelRegressions(bridge);
    if (!failures) std::puts("UI host-gesture regressions passed");
    return failures ? 1 : 0;
}

}  // namespace cd::plugin::test::gesture_regressions
