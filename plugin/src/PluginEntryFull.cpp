// Plugin factory for the full CircuitDrift product.
#include "PluginProcessor.h"

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new cd::plugin::CircuitDriftProcessor(cd::plugin::Edition::Full, "CircuitDrift");
}
