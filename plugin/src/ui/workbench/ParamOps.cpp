#include "ui/workbench/ParamOps.h"

#include <algorithm>
#include <cmath>

namespace cd::plugin::ui {

juce::RangedAudioParameter* ParamOps::param(const juce::String& baseName) const {
    // A gesture belongs to the parameters selected at its start. Changing the edit slot while
    // dragging must not redirect writes to a parameter with no matching host change gesture.
    if (active_) {
        for (const auto& t : touched_)
            if (t.baseName == baseName) return t.parameter;
        return nullptr;
    }
    return bridge_.parameters().getParameter(bridge_.slotParamId(baseName));
}

double ParamOps::normalized(const juce::String& baseName) const {
    auto* p = param(baseName);
    return p ? static_cast<double>(p->getValue()) : 0.0;
}

double ParamOps::real(const juce::String& baseName) const {
    auto* p = param(baseName);
    return p ? static_cast<double>(p->convertFrom0to1(p->getValue())) : 0.0;
}

double ParamOps::defaultNormalized(const juce::String& baseName) const {
    auto* p = param(baseName);
    return p ? static_cast<double>(p->getDefaultValue()) : 0.0;
}

bool ParamOps::begin(const juce::String& label, const std::vector<juce::String>& baseNames) {
    if (active_ || baseNames.empty()) return false;
    touched_.clear();
    for (const auto& n : baseNames) {
        auto* p = param(n);
        if (!p) { touched_.clear(); return false; }
        if (std::none_of(touched_.begin(), touched_.end(), [p](const auto& t) { return t.parameter == p; }))
            touched_.push_back({n, p, p->getValue()});
    }
    bridge_.beginGesture(label);
    for (auto& t : touched_) t.parameter->beginChangeGesture();
    active_ = true;
    return true;
}

void ParamOps::setNormalized(const juce::String& baseName, double normalized) {
    if (!active_ || !std::isfinite(normalized)) return;
    if (auto* p = param(baseName)) p->setValueNotifyingHost(static_cast<float>(std::clamp(normalized, 0.0, 1.0)));
}

void ParamOps::setReal(const juce::String& baseName, double plain) {
    if (!active_ || !std::isfinite(plain)) return;
    if (auto* p = param(baseName)) {
        const auto& range = p->getNormalisableRange();
        const auto bounded = std::clamp(plain, static_cast<double>(range.start), static_cast<double>(range.end));
        setNormalized(baseName, static_cast<double>(p->convertTo0to1(static_cast<float>(bounded))));
    }
}

void ParamOps::end() {
    if (!active_) return;
    for (auto& t : touched_) t.parameter->endChangeGesture();
    bridge_.endGesture();
    touched_.clear();
    active_ = false;
}

void ParamOps::rollback() {
    if (!active_) return;
    for (auto& t : touched_) t.parameter->setValueNotifyingHost(t.before);
    end();
}

bool ParamOps::setOnce(const juce::String& label, const juce::String& baseName, double normalizedValue) {
    if (!std::isfinite(normalizedValue)) return false;
    if (!begin(label, {baseName})) return false;
    setNormalized(baseName, normalizedValue);
    end();
    return true;
}

const char* editParamFor(int id) noexcept {
    switch (id) {
        case 2: return "stage.bias";               // V1
        case 12: return "supply.railCapacity";     // C_res
        case 1: case 3: case 4: case 9: case 10: case 14:  // R_g, R_load, R_k, R_dec, R_g2, R_filt
            return "condition.ageYears";
        default: return nullptr;
    }
}

}  // namespace cd::plugin::ui
