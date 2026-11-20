// Narrow test seam for the asynchronous native file chooser. All capture actions, status
// handling and rendering still run through the production panel and EditorCore.
#pragma once
#include "ui/panels/Panels.h"

namespace cd::plugin::ui::pc::testing {

bool setCaptureDestination(Panel& panel, const juce::File& destination);

}  // namespace cd::plugin::ui::pc::testing
