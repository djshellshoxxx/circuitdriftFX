// CircuitDrift plugin adapter: builds the host-visible parameter tree from the core registry.
// Ownership: returns a freshly built layout owned by the caller. Threading: message thread only
// (allocates); never call from the audio thread.
#pragma once

#include "Edition.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <cstddef>

namespace cd::core { struct ParamSpec; }

namespace cd::plugin {

// Version hint attached to every JUCE ParameterID. Bump only when a parameter's meaning changes.
inline constexpr int kParameterVersionHint = 1;

// True when the registry parameter is exposed by the given edition.
bool isExposed(const cd::core::ParamSpec& spec, Edition edition) noexcept;

// Number of registry parameters exposed by the edition (excludes the LITE wrapper bypass).
std::size_t exposedParameterCount(Edition edition) noexcept;

// Labels of an enum parameter (registry value min..max inclusive); empty for non-enum specs.
juce::StringArray enumLabels(const cd::core::ParamSpec& spec);

// Builds the layout: one juce parameter per exposed registry entry, string id == registry id,
// grouped by feature id (or slot A/B). LITE additionally gets a "bypass" wrapper parameter.
juce::AudioProcessorValueTreeState::ParameterLayout buildParameterLayout(Edition edition);

}  // namespace cd::plugin
