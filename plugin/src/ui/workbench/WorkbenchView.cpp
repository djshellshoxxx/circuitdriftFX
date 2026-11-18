// Temporary stub so the tree links; the workbench owner replaces this file.
#include "ui/workbench/WorkbenchView.h"

namespace cd::plugin::ui {
struct WorkbenchView::Impl { explicit Impl(EditorBridge& b) : bridge(b) {} EditorBridge& bridge; };
WorkbenchView::WorkbenchView(EditorBridge& bridge) : impl_(std::make_unique<Impl>(bridge)) {}
WorkbenchView::~WorkbenchView() = default;
void WorkbenchView::updateFrame(const engine::TelemetryFrame&, bool) {}
void WorkbenchView::settingsChanged(const UiSettings&) {}
void WorkbenchView::resized() {}
void WorkbenchView::paint(juce::Graphics&) {}
}  // namespace cd::plugin::ui
