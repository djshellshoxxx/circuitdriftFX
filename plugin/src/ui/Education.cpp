// Temporary stub so the tree links; the shell owner replaces this file with the full text bundle.
#include "ui/Education.h"

namespace cd::plugin::ui {
namespace {
const EducationEntry kEntries[] = {
    {"macro.age", "Age", "Ages every component together.", "", "", "", "Authored creative mapping (02-musical-controls)."},
};
}
const EducationEntry* findEducation(const juce::String& key) noexcept {
    juce::String k = key;
    if (k.startsWith("slot.a.") || k.startsWith("slot.b.")) k = k.substring(7);
    for (const auto& e : kEntries) if (k == e.key) return &e;
    return nullptr;
}
int educationCount() noexcept { return static_cast<int>(std::size(kEntries)); }
const EducationEntry& educationAt(int index) noexcept { return kEntries[index]; }
}  // namespace cd::plugin::ui
