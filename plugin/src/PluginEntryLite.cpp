// Plugin factory for the free CircuitDrift LITE product.
#include "PluginProcessor.h"

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new cd::plugin::CircuitDriftProcessor(cd::plugin::Edition::Lite, "CircuitDrift LITE");
}
