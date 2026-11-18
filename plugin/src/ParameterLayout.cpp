#include "ParameterLayout.h"

#include "EnumLabels.generated.inc"
#include "core/ParameterRegistry.h"

#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cd::plugin {
namespace {

using cd::core::ParamKind;
using cd::core::ParamMapping;
using cd::core::ParamScope;
using cd::core::ParamSpec;

struct EnumLabelSpan {
    const char* id;
    int offset;
    int count;
};
constexpr const char* kLabelPool[] = {CD_ENUM_LABEL_POOL};
constexpr EnumLabelSpan kLabelSpans[] = {CD_ENUM_LABEL_SPANS};

juce::String unitLabel(const ParamSpec& p) {
    static constexpr const char* kUnitless[] = {"normalized", "bool", "index", "bitmask", "odd-even"};
    for (const char* u : kUnitless)
        if (std::strcmp(p.unit, u) == 0) return {};
    return p.unit;
}

// Linear for ParamMapping::Linear; for Log the same law as cd::core::toPlain: min * (max/min)^u.
juce::NormalisableRange<float> rangeFor(const ParamSpec& p) {
    const auto lo = static_cast<float>(p.min);
    const auto hi = static_cast<float>(p.max);
    if (p.mapping != ParamMapping::Log) return {lo, hi};
    return {lo, hi,
            [](float start, float end, float u) { return start * std::pow(end / start, juce::jlimit(0.0f, 1.0f, u)); },
            [](float start, float end, float v) {
                return std::log(juce::jlimit(start, end, v) / start) / std::log(end / start);
            },
            [](float start, float end, float v) { return juce::jlimit(start, end, v); }};
}

juce::String groupKey(const ParamSpec& p) {
    if (p.scope == ParamScope::SlotA) return "slotA";
    if (p.scope == ParamScope::SlotB) return "slotB";
    return juce::String(p.featureId).toLowerCase();
}

juce::String groupName(const ParamSpec& p) {
    if (p.scope == ParamScope::SlotA) return "Slot A";
    if (p.scope == ParamScope::SlotB) return "Slot B";
    return p.featureId;
}

std::unique_ptr<juce::RangedAudioParameter> makeParameter(const ParamSpec& p) {
    const juce::ParameterID pid{p.id, kParameterVersionHint};
    const juce::String name{p.id};  // display names arrive with the UI copy; the id is unambiguous
    switch (p.kind) {
        case ParamKind::Float:
            return std::make_unique<juce::AudioParameterFloat>(
                pid, name, rangeFor(p), static_cast<float>(p.def),
                juce::AudioParameterFloatAttributes().withLabel(unitLabel(p)).withAutomatable(p.automatable));
        case ParamKind::Int:
            return std::make_unique<juce::AudioParameterInt>(
                pid, name, static_cast<int>(p.min), static_cast<int>(p.max), static_cast<int>(p.def),
                juce::AudioParameterIntAttributes().withLabel(unitLabel(p)).withAutomatable(p.automatable));
        case ParamKind::Bool:
            return std::make_unique<juce::AudioParameterBool>(
                pid, name, p.def >= 0.5, juce::AudioParameterBoolAttributes().withAutomatable(p.automatable));
        case ParamKind::Enum:
            return std::make_unique<juce::AudioParameterChoice>(
                pid, name, enumLabels(p), static_cast<int>(std::lround(p.def - p.min)),
                juce::AudioParameterChoiceAttributes().withAutomatable(p.automatable));
    }
    return nullptr;
}

}  // namespace

bool isExposed(const ParamSpec& spec, Edition edition) noexcept {
    const bool lite = std::strncmp(spec.id, kLiteIdPrefix, std::strlen(kLiteIdPrefix)) == 0;
    return edition == Edition::Lite ? lite : !lite;
}

std::size_t exposedParameterCount(Edition edition) noexcept {
    std::size_t n = 0;
    for (const auto& p : cd::core::kParams)
        if (isExposed(p, edition)) ++n;
    return n;
}

juce::StringArray enumLabels(const ParamSpec& spec) {
    juce::StringArray labels;
    if (spec.kind != ParamKind::Enum) return labels;
    for (const auto& s : kLabelSpans) {
        if (std::strcmp(s.id, spec.id) != 0) continue;
        for (int i = 0; i < s.count; ++i) labels.add(kLabelPool[s.offset + i]);
    }
    return labels;
}

juce::AudioProcessorValueTreeState::ParameterLayout buildParameterLayout(Edition edition) {
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // Registry order is preserved inside groups; groups appear in order of first use.
    std::vector<std::string> order;
    std::map<std::string, std::unique_ptr<juce::AudioProcessorParameterGroup>> groups;
    for (const auto& p : cd::core::kParams) {
        if (!isExposed(p, edition)) continue;
        const auto key = groupKey(p).toStdString();
        auto& group = groups[key];
        if (!group) {
            group = std::make_unique<juce::AudioProcessorParameterGroup>(key, groupName(p), " | ");
            order.push_back(key);
        }
        group->addChild(makeParameter(p));
    }
    for (const auto& key : order) layout.add(std::move(groups[key]));

    if (edition == Edition::Lite)
        layout.add(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{kBypassParamId, kParameterVersionHint}, "Bypass", false));
    return layout;
}

}  // namespace cd::plugin
