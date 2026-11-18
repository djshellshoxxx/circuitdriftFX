// CircuitDrift plugin: bundled, offline tooltip/education text (CD-F08). Contract owned by the
// coordinator; entries/implementation by the shell owner (plugin/src/ui/Education.cpp).
// Keys: engine component keys ("C_res", "V1", ...) and registry parameter ids ("macro.age",
// "slot.a.stage.drive" -> looked up by base name "stage.drive"). Static storage, any thread.
#pragma once
#include <juce_core/juce_core.h>

namespace cd::plugin::ui {

struct EducationEntry {
    const char* key;
    const char* title;
    const char* shortText;    // <= 60 words
    const char* longText;     // <= 250 words, educational mode
    const char* audioEffect;  // "What changes in audio"
    const char* units;        // display units, "" if none
    const char* source;       // model/source note, e.g. "Synthetic fixture (G01); see docs/..."
};

// nullptr if no entry. Slot-prefixed parameter ids are normalised to their base name.
const EducationEntry* findEducation(const juce::String& key) noexcept;
// Every bundled entry (for coverage tests).
int educationCount() noexcept;
const EducationEntry& educationAt(int index) noexcept;

}  // namespace cd::plugin::ui
