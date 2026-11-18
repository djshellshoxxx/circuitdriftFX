#include "ui/workbench/PcbCanvas.h"

#include <cmath>
#include "ui/Education.h"
#include "ui/analysis/AnalysisDisplays.h"

namespace cd::plugin::ui {
namespace {
using juce::Colour;
using juce::Graphics;
using juce::Point;
using juce::Rectangle;
using pcb::Kind;
using engine::ComponentId;

constexpr double kPi = 3.14159265358979323846;
constexpr int kMaterialCount = 3;

juce::Font fnt(float h, bool bold = false) {
    juce::Font f{juce::FontOptions(h)};
    return bold ? f.boldened() : f;
}
float strW(const juce::Font& f, const juce::String& s) {
    juce::GlyphArrangement ga;
    ga.addLineOfText(f, s, 0.0f, 0.0f);
    return ga.getBoundingBox(0, -1, true).getWidth();
}
juce::String degC() { return juce::String(juce::CharPointer_UTF8("\xC2\xB0")) + "C"; }
bool isRound(Kind k) { return k == Kind::Tube || k == Kind::Electrolytic; }
float smooth(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
}  // namespace

PcbCanvas::PcbCanvas(EditorBridge& bridge) : bridge_(bridge), ops_(bridge) {
    setWantsKeyboardFocus(true);
    setTitle("PCB workbench");
    setDescription("Interactive board of the factory circuit. Use [ and ] to choose a component, arrow keys to adjust it, Home to reset, Escape to cancel a gesture.");
    setOpaque(true);
    proj_.setViewport(400, 270);
    for (auto& r : readouts_) r = PartReadout{};
    refreshReadouts();
}

PcbCanvas::~PcbCanvas() { stopTimer(); cancelOrCommitOnLoss(); }

void PcbCanvas::cancelOrCommitOnLoss() {
    // Pointer/visibility loss commits the last valid value and ends host gestures (UX-01): no stuck automation.
    if (burstActive_) endBurst();
    if (drag_ == Drag::Bias || drag_ == Drag::Supply || drag_ == Drag::Age) { publishDragValue(true); ops_.end(); }
    drag_ = Drag::None;
}

// ---- state --------------------------------------------------------------------------------------------
void PcbCanvas::applySettings(const UiSettings& s) {
    settings_ = s;
    theme_ = &themeFor(s.darkTheme);
    overlay_ = std::clamp(s.overlay, 0, 3);
    xray_ = s.xray;
    reduceMotion_ = s.reduceMotion;
    repaint();
}

void PcbCanvas::ingest(const engine::TelemetryFrame& f, bool valid, double nowMs) {
    valid_ = valid;
    if (valid) {
        if (frame_.sampleIndex > f.sampleIndex && frame_.generation != 0) staleTracker_.reset();  // seek / graph change
        frame_ = f;
    }
    nowMs_ = nowMs;
    staleTracker_.update(f.generation, f.sampleIndex, valid, nowMs);
    stale_ = staleOverride_ >= 0 ? staleOverride_ == 1 : staleTracker_.stale(nowMs);
    slot_ = std::clamp(bridge_.status().editSlot, 0, engine::kSlots - 1);
    eff_ = bridge_.effectiveTargets(slot_);
    for (int ch = 0; ch < engine::kMaxChannels; ++ch) runMaxCurrent_ = std::max(std::abs(frame_.slot[slot_].stageCurrentA[ch]), runMaxCurrent_ * 0.995);
    runMaxCurrent_ = std::max(runMaxCurrent_, 1e-6);
    ++frameCounter_;
    refreshReadouts();
    repaint();
}

void PcbCanvas::refreshReadouts() {
    ReadoutContext c;
    c.bridge = &bridge_;
    c.frame = &frame_;
    c.frameValid = valid_ || frame_.generation != 0;
    c.slot = slot_;
    c.channel = channel_;
    c.eff = eff_;
    c.faults = bridge_.faults(slot_);
    for (int i = 0; i < pcb::kPartCount; ++i) readouts_[static_cast<std::size_t>(i)] = readPart(i, c);
}

void PcbCanvas::setTool(Tool t) { tool_ = t; repaint(); if (onStateChanged) onStateChanged(); }

void PcbCanvas::setViewMode(ViewMode m) {
    mode_ = m;
    proj_.resetView();
    if (m == ViewMode::Board3D) proj_.setOrbit(-0.55, 1.0); else proj_.setOrbit(0.0, kPi / 2);
    updateProjection();
    repaint();
    if (onStateChanged) onStateChanged();
}

void PcbCanvas::updateProjection() {
    proj_.setViewport(std::max(1, getWidth()), std::max(1, getHeight() - kLegendH));
    proj_.setExtrusion(mode_ == ViewMode::Board3D ? 14.0 : 0.0);
}
void PcbCanvas::resized() { updateProjection(); }

void PcbCanvas::select(int id) {
    id = id >= 0 && id < pcb::kPartCount ? id : -1;
    if (id == selected_) return;
    selected_ = id;
    if (id >= 0) {
        const auto* info = engine::findComponent(pcb::parts()[static_cast<std::size_t>(id)].key);
        setDescription(juce::String("Selected ") + (info ? info->displayName : pcb::parts()[static_cast<std::size_t>(id)].key));
    }
    if (onSelect) onSelect(id);
    repaint();
}

void PcbCanvas::cycleSelection(int delta) {
    int next = selected_ < 0 ? (delta > 0 ? 0 : pcb::kPartCount - 1) : (selected_ + delta + pcb::kPartCount) % pcb::kPartCount;
    select(next);
}

bool PcbCanvas::isSupplyTarget(int id) const noexcept {
    return id == static_cast<int>(ComponentId::C_res) || id == static_cast<int>(ComponentId::C_filt) || id == static_cast<int>(ComponentId::T_out);
}
bool PcbCanvas::isResistor(int id) const noexcept { return id >= 0 && pcb::parts()[static_cast<std::size_t>(id)].kind == Kind::Resistor; }

// ---- hit testing --------------------------------------------------------------------------------------
int PcbCanvas::hitTest(Point<float> p) const {
    const auto c = pcb::hitCandidates(proj_, heightScale(), p.x, p.y, gesture::kMinHitPx);
    return c.empty() ? -1 : c.front();
}

int PcbCanvas::cyclePick(Point<float> p) {
    const auto c = pcb::hitCandidates(proj_, heightScale(), p.x, p.y, gesture::kMinHitPx);
    if (c.empty()) { lastClickPos_ = p; return -1; }
    int pick = c.front();
    // Stacked objects: repeated click at the same spot cycles through the candidates by depth.
    if (c.size() > 1 && selected_ >= 0 && p.getDistanceFrom(lastClickPos_) < 6.0f) {
        for (std::size_t i = 0; i < c.size(); ++i)
            if (c[i] == selected_) { pick = c[(i + 1) % c.size()]; break; }
    }
    lastClickPos_ = p;
    return pick;
}

Point<float> PcbCanvas::centreOf(int id) const {
    const auto& p = pcb::parts()[static_cast<std::size_t>(id)];
    return proj(p.x + p.w * 0.5, p.y + p.h * 0.5, p.height * heightScale());
}

Point<float> PcbCanvas::proj(double x, double y, double z) const {
    const auto p = proj_.project(x, y, z);
    return {static_cast<float>(p.x), static_cast<float>(p.y)};
}

juce::AffineTransform PcbCanvas::planeTransform(double z) const {
    double a, b, c, d, e, f;
    proj_.plane(z, a, b, c, d, e, f);
    return juce::AffineTransform(static_cast<float>(a), static_cast<float>(b), static_cast<float>(c),
                                 static_cast<float>(d), static_cast<float>(e), static_cast<float>(f));
}

// ---- gestures -----------------------------------------------------------------------------------------
bool PcbCanvas::pointerDown(Point<float> p, juce::ModifierKeys mods) {
    cancelOrCommitOnLoss();
    downPos_ = lastPos_ = p;
    moved_ = false;
    pathSigned_ = 0;
    brushSet_.clear();
    readout_.clear();
    if (mods.isMiddleButtonDown() || mods.isRightButtonDown()) { drag_ = Drag::Pan; return true; }
    const int hit = cyclePick(p);
    if (hit < 0) { drag_ = (mode_ == ViewMode::Board3D && !mods.isShiftDown()) ? Drag::Orbit : Drag::Pan; return true; }
    select(hit);
    const auto kind = pcb::parts()[static_cast<std::size_t>(hit)].kind;
    if (tool_ == Tool::Material && isResistor(hit)) { showMaterialMenu(hit); return true; }
    if ((tool_ == Tool::Bias || tool_ == Tool::Auto) && kind == Kind::Tube) {
        if (ops_.begin("Tube bias (drag)", {"stage.bias"})) {
            startNorm_ = pendingNorm_ = ops_.normalized("stage.bias");
            pendingParam_ = "stage.bias";
            drag_ = Drag::Bias;
        }
        return true;
    }
    if ((tool_ == Tool::Supply || tool_ == Tool::Auto) && isSupplyTarget(hit)) {
        if (ops_.begin("Supply capacity (stretch)", {"supply.railCapacity"})) {
            startC_ = ops_.real("supply.railCapacity");
            pendingParam_ = "supply.railCapacity";
            drag_ = Drag::Supply;
        }
        return true;
    }
    if ((tool_ == Tool::AgeBrush || tool_ == Tool::Auto) && isResistor(hit)) {
        // The brush set is frozen at pointer-down: the resistors within the brush radius (never re-picked).
        for (const auto& part : pcb::parts())
            if (part.kind == Kind::Resistor && (part.id == hit || centreOf(part.id).getDistanceFrom(p) < 90.0f)) brushSet_.push_back(part.id);
        if (ops_.begin("Age brush (resistors)", {"condition.ageYears"})) {
            startNorm_ = pendingNorm_ = ops_.normalized("condition.ageYears");
            pendingParam_ = "condition.ageYears";
            drag_ = Drag::Age;
        }
        return true;
    }
    return true;
}

void PcbCanvas::publishDragValue(bool force) {
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (force || now - lastPublishMs_ >= 8.3) {  // <= 120 Hz target publication
        ops_.setNormalized(pendingParam_, pendingNorm_);
        lastPublishMs_ = now;
    }
}

void PcbCanvas::pointerDrag(Point<float> p, juce::ModifierKeys mods) {
    const auto d = p - lastPos_;
    if (p.getDistanceFrom(downPos_) > 2.0f) moved_ = true;
    switch (drag_) {
        case Drag::Orbit:
            proj_.setOrbit(proj_.yaw() + d.x * 0.008, std::clamp(proj_.pitch() - d.y * 0.008, 0.35, 1.45));
            break;
        case Drag::Pan:
            proj_.addPan(d.x, d.y);
            break;
        case Drag::Bias:
            // Logical pixels: event coordinates under the editor's scale transform already are logical.
            pendingNorm_ = gesture::biasFromDrag(pendingNorm_, d.y, mods.isShiftDown());
            publishDragValue(false);
            readout_ = "Bias " + juce::String(pendingNorm_ * 2.0 - 1.0, 2) + " (normalized " + juce::String(pendingNorm_, 2) + ")";
            break;
        case Drag::Supply: {
            auto* prm = ops_.param("supply.railCapacity");
            const auto range = prm->getNormalisableRange().getRange();
            const double r = std::pow(2.0, (p.x - downPos_.x) / 150.0);
            const double c = gesture::supplyCapacitance(startC_, r, range.getStart(), range.getEnd());
            pendingNorm_ = prm->convertTo0to1(static_cast<float>(c));
            publishDragValue(false);
            readout_ = "Reservoir " + formatSi(c * 1e-6, "F") + "  (x" + juce::String(c / std::max(startC_, 1e-9), 2) + ", sag " + juce::String(gesture::sagContribution(r), 2) + ")";
            break;
        }
        case Drag::Age:
            pathSigned_ += (mods.isAltDown() ? -1.0 : 1.0) * static_cast<double>(d.getDistanceFromOrigin());
            pendingNorm_ = gesture::ageFromPath(startNorm_, pathSigned_, false);
            publishDragValue(false);
            readout_ = juce::String(mods.isAltDown() ? "Age (reverse) " : "Age ") + juce::String(pendingNorm_ * 100.0, 0) + " %";
            break;
        case Drag::None: break;
    }
    lastPos_ = p;
    repaint();
}

void PcbCanvas::pointerUp(Point<float>) {
    if (drag_ == Drag::Bias || drag_ == Drag::Supply || drag_ == Drag::Age) {
        publishDragValue(true);
        ops_.end();
    } else if ((drag_ == Drag::Orbit || drag_ == Drag::Pan) && !moved_) {
        select(-1);
    }
    drag_ = Drag::None;
    brushSet_.clear();
    readout_.clear();
    repaint();
}

void PcbCanvas::cancelGesture() {
    if (burstActive_) { ops_.rollback(); burstActive_ = false; stopTimer(); }
    else if (drag_ == Drag::Bias || drag_ == Drag::Supply || drag_ == Drag::Age) ops_.rollback();
    drag_ = Drag::None;
    readout_.clear();
    repaint();
}

void PcbCanvas::supplyBurst(double ratio) {
    if (!isSupplyTarget(selected_)) return;
    if (!burstActive_) {
        if (!ops_.begin("Supply capacity (scroll/pinch)", {"supply.railCapacity"})) return;
        burstActive_ = true;
        burstStartC_ = ops_.real("supply.railCapacity");
        burstRatio_ = 1.0;
    }
    burstRatio_ = gesture::clampPinch(burstRatio_ * ratio);   // r clamped 0.5..2 per gesture
    auto* prm = ops_.param("supply.railCapacity");
    const auto range = prm->getNormalisableRange().getRange();
    const double c = gesture::supplyCapacitance(burstStartC_, burstRatio_, range.getStart(), range.getEnd());
    ops_.setReal("supply.railCapacity", c);
    readout_ = "Reservoir " + formatSi(c * 1e-6, "F") + "  (r " + juce::String(burstRatio_, 2) + ", sag " + juce::String(gesture::sagContribution(burstRatio_), 2) + ")";
    startTimer(350);
    repaint();
}

void PcbCanvas::endBurst() {
    stopTimer();
    if (burstActive_) { ops_.end(); burstActive_ = false; }
    readout_.clear();
    repaint();
}

void PcbCanvas::adjustSelected(int direction, bool fine) {
    if (selected_ < 0) return;
    if (tool_ == Tool::Material && isResistor(selected_)) {
        const int cur = static_cast<int>(std::lround(ops_.real("condition.resistorType")));
        setMaterial((cur + direction + kMaterialCount) % kMaterialCount);
        return;
    }
    const char* base = editParamFor(selected_);
    if (!base) return;
    ops_.setOnce(juce::String("Adjust ") + pcb::parts()[static_cast<std::size_t>(selected_)].key, base, gesture::keyAdjust(ops_.normalized(base), direction, fine));
    refreshReadouts();
    repaint();
}

void PcbCanvas::resetSelected() {
    if (selected_ < 0) return;
    if (tool_ == Tool::Material && isResistor(selected_)) { setMaterial(1); return; }
    const char* base = editParamFor(selected_);
    if (!base) return;
    ops_.setOnce(juce::String("Reset ") + pcb::parts()[static_cast<std::size_t>(selected_)].key, base, ops_.defaultNormalized(base));
    refreshReadouts();
    repaint();
}

void PcbCanvas::setMaterial(int index) {
    index = std::clamp(index, 0, kMaterialCount - 1);
    auto* p = ops_.param("condition.resistorType");
    if (!p) return;
    // Explicit enum choice applied once; no interpolation between labels.
    ops_.setOnce(juce::String("Resistor material: ") + materialName(index), "condition.resistorType", p->convertTo0to1(static_cast<float>(index)));
    refreshReadouts();
    repaint();
}

void PcbCanvas::showMaterialMenu(int id) {
    juce::PopupMenu m;
    const int cur = static_cast<int>(std::lround(ops_.real("condition.resistorType")));
    for (int i = 0; i < kMaterialCount; ++i) m.addItem(i + 1, materialName(i), true, i == cur);
    const auto area = localAreaToGlobal(Rectangle<int>::leftTopRightBottom(static_cast<int>(centreOf(id).x), static_cast<int>(centreOf(id).y), static_cast<int>(centreOf(id).x) + 1, static_cast<int>(centreOf(id).y) + 1));
    juce::Component::SafePointer<PcbCanvas> safe(this);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(area), [safe](int r) {
        if (safe != nullptr && r > 0) safe->setMaterial(r - 1);
    });
}

// ---- mouse / keyboard glue -----------------------------------------------------------------------------
void PcbCanvas::mouseDown(const juce::MouseEvent& e) {
    grabKeyboardFocus();
    pointerDown(e.position, e.mods);
}
void PcbCanvas::mouseDrag(const juce::MouseEvent& e) { pointerDrag(e.position, e.mods); }
void PcbCanvas::mouseUp(const juce::MouseEvent& e) { pointerUp(e.position); }

juce::String PcbCanvas::hintFor(int id) const {
    const auto& part = pcb::parts()[static_cast<std::size_t>(id)];
    const auto* info = engine::findComponent(part.key);
    juce::String s = juce::String(info ? info->displayName : part.key);
    if (part.kind == Kind::Tube) s += ": drag up/down = bias (200 px = full range, Shift fine)";
    else if (isSupplyTarget(id)) s += ": drag sideways, scroll or pinch = reservoir capacity (changes supply.railCapacity)";
    else if (part.kind == Kind::Resistor) s += tool_ == Tool::Material ? ": click = choose material" : ": drag = age brush (changes slot age + linked noise; Alt reverses)";
    if (const auto* e = findEducation(part.key)) s += juce::String("\n") + e->shortText;
    return s;
}

void PcbCanvas::updateHover(Point<float> p) {
    const int h = hitTest(p);
    if (h != hovered_) {
        hovered_ = h;
        setTooltip(h >= 0 ? hintFor(h) : juce::String());
        if (h >= 0) {
            const auto k = pcb::parts()[static_cast<std::size_t>(h)].kind;
            setMouseCursor(k == Kind::Tube ? juce::MouseCursor::UpDownResizeCursor
                           : isSupplyTarget(h) ? juce::MouseCursor::LeftRightResizeCursor
                           : isResistor(h) ? juce::MouseCursor::CrosshairCursor : juce::MouseCursor::PointingHandCursor);
        } else {
            setMouseCursor(mode_ == ViewMode::Board3D ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
        }
        repaint();
    }
}
void PcbCanvas::mouseMove(const juce::MouseEvent& e) { updateHover(e.position); }
void PcbCanvas::mouseExit(const juce::MouseEvent&) { hovered_ = -1; setTooltip({}); repaint(); }

void PcbCanvas::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) {
    if ((tool_ == Tool::Supply || tool_ == Tool::Auto) && isSupplyTarget(selected_) && hitTest(e.position) == selected_) {
        supplyBurst(gesture::wheelRatio(w.deltaY));
        return;
    }
    const double z0 = proj_.zoom();
    proj_.setZoom(z0 * std::pow(1.15, w.deltaY * 2.0));
    const double z1 = proj_.zoom();
    const Point<double> c(getWidth() * 0.5, getHeight() * 0.5);
    // Keep the point under the cursor fixed while zooming.
    const double k = 1.0 - z1 / z0;
    proj_.addPan((e.position.x - c.x) * k - (0.0), (e.position.y - c.y) * k - (0.0));
    repaint();
}

void PcbCanvas::mouseMagnify(const juce::MouseEvent& e, float scaleFactor) {
    if ((tool_ == Tool::Supply || tool_ == Tool::Auto) && isSupplyTarget(selected_) && hitTest(e.position) == selected_) {
        supplyBurst(scaleFactor);
        return;
    }
    proj_.setZoom(proj_.zoom() * scaleFactor);
    repaint();
}

bool PcbCanvas::keyPressed(const juce::KeyPress& k) {
    const bool shift = k.getModifiers().isShiftDown();
    if (k == juce::KeyPress::escapeKey) {
        if (gestureActive()) { cancelGesture(); return true; }
        return false;
    }
    const auto ch = k.getTextCharacter();
    if (ch == '[' || k == juce::KeyPress::pageUpKey) { cycleSelection(-1); return true; }
    if (ch == ']' || k == juce::KeyPress::pageDownKey) { cycleSelection(+1); return true; }
    if (selected_ < 0) return false;
    if (k.isKeyCode(juce::KeyPress::upKey) || k.isKeyCode(juce::KeyPress::rightKey)) { adjustSelected(+1, shift); return true; }
    if (k.isKeyCode(juce::KeyPress::downKey) || k.isKeyCode(juce::KeyPress::leftKey)) { adjustSelected(-1, shift); return true; }
    if (k.isKeyCode(juce::KeyPress::homeKey)) { resetSelected(); return true; }
    return false;
}

// ---- painting -----------------------------------------------------------------------------------------
Colour PcbCanvas::bodyColour(const pcb::Part& part) const {
    switch (part.kind) {
        case Kind::Resistor: return Colour(0xffd2b48c);
        case Kind::FilmCap: return Colour(0xffe0a030);
        case Kind::Electrolytic: return Colour(0xff23395b);
        case Kind::Tube: return Colour(0xff9fb8c8);
        case Kind::Transformer: return Colour(0xff59616b);
        case Kind::Diode: return Colour(0xff25252b);
        case Kind::Chassis: return Colour(0xff808891);
    }
    return Colour(0xff888888);
}

Colour PcbCanvas::tintFor(const pcb::Part& part, bool& hatch) const {
    hatch = false;
    const auto& r = readouts_[static_cast<std::size_t>(part.id)];
    if (overlay_ == 1 && r.hasTemp) return analysis::thermalColour(*theme_, pcb::thermalPosition(part.ref, r.tempC));
    if (overlay_ == 2) {
        if (r.hasVoltage) return analysis::signedColour(*theme_, r.voltage / std::max(r.voltageScale, 1e-9));
        hatch = true;
    }
    return Colour();
}

void PcbCanvas::paint(Graphics& g) {
    const auto& t = *theme_;
    g.fillAll(t.inset);
    updateProjection();
    if (mode_ == ViewMode::Schematic) drawSchematic(g); else drawBoard(g);
    drawFlows(g);
    drawLabels(g);
    drawSelection(g);
    drawLegend(g);

    if (readout_.isNotEmpty()) {
        const auto anchor = lastPos_;
        g.setFont(fnt(12.0f, true));
        const float w = strW(g.getCurrentFont(), readout_) + 14.0f;
        auto r = Rectangle<float>(std::clamp(anchor.x + 14.0f, 4.0f, getWidth() - w - 4.0f), std::clamp(anchor.y - 28.0f, 4.0f, getHeight() - 24.0f), w, 20.0f);
        g.setColour(t.panelRaised.withAlpha(0.95f));
        g.fillRoundedRectangle(r, 4.0f);
        g.setColour(t.focus);
        g.drawRoundedRectangle(r, 4.0f, 1.5f);
        g.setColour(t.text);
        g.drawText(readout_, r, juce::Justification::centred);
    }
    if (stale_) {
        const auto badge = Rectangle<float>(getWidth() - 96.0f, 6.0f, 90.0f, 18.0f);
        g.setColour(t.warning);
        g.fillRoundedRectangle(badge, 4.0f);
        g.setColour(t.textInverse);
        g.setFont(fnt(11.0f, true));
        g.drawText("! STALE DATA", badge, juce::Justification::centred);
    }
    if (hasKeyboardFocus(false)) {
        g.setColour(t.focus);
        g.drawRect(getLocalBounds().toFloat().reduced(1.0f), t.focusOutline);
    }
}

void PcbCanvas::drawTraces(Graphics& g, bool schematic) {
    const auto& t = *theme_;
    g.saveState();
    g.addTransform(planeTransform(0.05));
    const auto& rd = readouts_[static_cast<std::size_t>(ComponentId::C_res)];
    for (const auto& tr : pcb::traces()) {
        if (tr.bottomLayer && !xray_ && !schematic) { /* drawn faint below */ }
        juce::Path p;
        for (std::size_t i = 0; i < tr.pts.size(); ++i) {
            if (i == 0) p.startNewSubPath(static_cast<float>(tr.pts[i].x), static_cast<float>(tr.pts[i].y));
            else p.lineTo(static_cast<float>(tr.pts[i].x), static_cast<float>(tr.pts[i].y));
        }
        Colour c;
        float w = 1.5f;
        switch (tr.net) {
            case pcb::Net::Signal: c = t.pcbTrace; break;
            case pcb::Net::Rail: c = t.pcbCopper.brighter(0.2f); w = 1.9f; break;
            case pcb::Net::Ground: c = t.pcbTrace.darker(0.4f); w = 2.2f; break;
            case pcb::Net::Screen: c = t.pcbTrace.darker(0.15f); w = 1.2f; break;
        }
        if (overlay_ == 2 && tr.net == pcb::Net::Rail && rd.hasVoltage)
            c = analysis::signedColour(t, rd.voltage / std::max(rd.voltageScale, 1e-9)).brighter(0.15f);
        if (schematic) { c = t.textDim; w = 0.45f; }
        if (tr.bottomLayer) c = c.withAlpha(xray_ ? 0.9f : 0.45f);
        if (overlay_ == 1 || overlay_ == 3) c = c.withAlpha(c.getFloatAlpha() * 0.75f);
        g.setColour(c);
        if (tr.bottomLayer && xray_) {
            juce::Path d;
            const float dashes[2] = {2.5f, 1.8f};
            juce::PathStrokeType(w).createDashedStroke(d, p, dashes, 2);
            g.fillPath(d);
        } else {
            g.strokePath(p, juce::PathStrokeType(w, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded));
        }
        if (!schematic) {  // solder pads at the trace ends
            g.setColour(t.pcbCopper.brighter(0.35f));
            for (const auto& pt : {tr.pts.front(), tr.pts.back()}) g.fillEllipse(static_cast<float>(pt.x) - 1.1f, static_cast<float>(pt.y) - 1.1f, 2.2f, 2.2f);
        }
    }
    g.restoreState();
}

void PcbCanvas::drawBoard(Graphics& g) {
    const auto& t = *theme_;
    const double W = pcb::kBoardW, H = pcb::kBoardH, TH = pcb::kBoardThickness;
    const bool solid = mode_ == ViewMode::Board3D;
    const Colour sub = t.pcbSubstrate;

    // Slab sides (oblique view only).
    if (solid) {
        struct Edge { double x0, y0, x1, y1, nx, ny; };
        const Edge edges[4] = {{0, 0, W, 0, 0, -1}, {W, 0, W, H, 1, 0}, {W, H, 0, H, 0, 1}, {0, H, 0, 0, -1, 0}};
        for (const auto& e : edges) {
            if (proj_.faceFacing(e.nx, e.ny) <= 0.02) continue;
            juce::Path p;
            p.startNewSubPath(proj(e.x0, e.y0, 0)); p.lineTo(proj(e.x1, e.y1, 0));
            p.lineTo(proj(e.x1, e.y1, -TH)); p.lineTo(proj(e.x0, e.y0, -TH)); p.closeSubPath();
            g.setColour(sub.darker(0.55f));
            g.fillPath(p);
            g.setColour(sub.darker(0.8f));
            g.strokePath(p, juce::PathStrokeType(1.0f));
        }
        // Soft ground shadow under the slab.
        juce::Path sh;
        sh.startNewSubPath(proj(2, 2, -TH)); sh.lineTo(proj(W + 4, 4, -TH)); sh.lineTo(proj(W + 4, H + 4, -TH)); sh.lineTo(proj(4, H + 4, -TH)); sh.closeSubPath();
        g.setColour(juce::Colours::black.withAlpha(0.18f));
        g.fillPath(sh);
    }
    // Substrate top.
    juce::Path top;
    top.startNewSubPath(proj(0, 0, 0)); top.lineTo(proj(W, 0, 0)); top.lineTo(proj(W, H, 0)); top.lineTo(proj(0, H, 0)); top.closeSubPath();
    g.setGradientFill(juce::ColourGradient(sub.brighter(0.12f), proj(0, 0, 0), sub.darker(0.15f), proj(W, H, 0), false));
    g.fillPath(top);
    g.setColour(sub.brighter(0.5f).withAlpha(0.8f));
    g.strokePath(top, juce::PathStrokeType(1.2f));

    // Silkscreen: outline, mounting holes, section labels (drawn on the board plane).
    g.saveState();
    g.addTransform(planeTransform(0.02));
    g.setColour(t.pcbSilk.withAlpha(0.55f));
    g.drawRoundedRectangle(1.8f, 1.8f, static_cast<float>(W) - 3.6f, static_cast<float>(H) - 3.6f, 2.0f, 0.35f);
    for (auto hole : {Point<float>(5.0f, 78.0f), Point<float>(125.0f, 78.0f), Point<float>(125.0f, 22.0f), Point<float>(5.0f, 50.0f)}) {
        g.setColour(t.pcbSubstrate.darker(0.8f));
        g.fillEllipse(hole.x - 1.6f, hole.y - 1.6f, 3.2f, 3.2f);
        g.setColour(t.pcbCopper);
        g.drawEllipse(hole.x - 2.4f, hole.y - 2.4f, 4.8f, 4.8f, 0.5f);
    }
    g.setColour(t.pcbSilk.withAlpha(0.45f));
    g.setFont(fnt(3.2f, true));
    g.drawText("SIGNAL", Rectangle<float>(30, 37, 14, 4), juce::Justification::centredLeft);
    g.drawText("POWER SUPPLY", Rectangle<float>(26, 77, 50, 4), juce::Justification::centredLeft);
    g.drawText("CIRCUITDRIFT  REV A  (illustrative layout)", Rectangle<float>(60, 77, 66, 4), juce::Justification::centredRight);
    g.setFont(fnt(2.8f));
    g.drawText("IN", Rectangle<float>(0.5f, 34.0f, 7, 3), juce::Justification::centredLeft);
    g.drawText("OUT", Rectangle<float>(121.0f, 34.0f, 8, 3), juce::Justification::centredLeft);
    g.drawText("AC", Rectangle<float>(0.5f, 69.5f, 7, 3), juce::Justification::centredLeft);
    g.drawText("B+", Rectangle<float>(119.0f, 0.8f, 8, 3), juce::Justification::centredLeft);
    g.drawText("GND", Rectangle<float>(104.0f, 59.0f, 12, 3), juce::Justification::centredLeft);
    g.restoreState();

    drawTraces(g, false);

    // Parts back to front.
    std::vector<std::pair<double, int>> order;
    for (const auto& p : pcb::parts()) order.emplace_back(proj_.depth(p.x + p.w * 0.5, p.y + p.h * 0.5, 0.0), p.id);
    std::sort(order.begin(), order.end());
    for (const auto& o : order) drawPart(g, pcb::parts()[static_cast<std::size_t>(o.second)]);
}

void PcbCanvas::drawSchematic(Graphics& g) {
    const auto& t = *theme_;
    g.setColour(t.panel);
    g.fillRect(getLocalBounds());
    // faint drawing-sheet grid
    g.setColour(t.grid.withAlpha(0.25f));
    for (double x = 0; x <= pcb::kBoardW; x += 10) { const auto a = proj(x, 0, 0), b = proj(x, pcb::kBoardH, 0); g.drawLine(a.x, a.y, b.x, b.y, 0.5f); }
    for (double y = 0; y <= pcb::kBoardH; y += 10) { const auto a = proj(0, y, 0), b = proj(pcb::kBoardW, y, 0); g.drawLine(a.x, a.y, b.x, b.y, 0.5f); }
    drawTraces(g, true);
    for (const auto& p : pcb::parts()) drawSymbol(g, p);
    g.setColour(t.textDim);
    g.setFont(fnt(11.0f, true));
    const auto in = proj(0, 33, 0), out = proj(pcb::kBoardW, 33, 0), rail = proj(118, 4, 0), gnd = proj(110, 58, 0);
    g.drawText("IN", Rectangle<float>(in.x, in.y - 16, 30, 14), juce::Justification::centredLeft);
    g.drawText("OUT", Rectangle<float>(out.x - 36, out.y - 16, 30, 14), juce::Justification::centredRight);
    g.drawText("B+ rail", Rectangle<float>(rail.x - 50, rail.y - 16, 56, 14), juce::Justification::centredRight);
    g.drawText("GND", Rectangle<float>(gnd.x, gnd.y - 14, 36, 14), juce::Justification::centredLeft);
}

void PcbCanvas::drawPart(Graphics& g, const pcb::Part& part) {
    const auto& t = *theme_;
    const double hs = heightScale(), h = part.height * hs;
    const float alpha = xray_ ? 0.34f : (overlay_ == 3 ? 0.62f : 1.0f);
    bool hatch = false;
    const Colour tint = tintFor(part, hatch);
    const Colour base = bodyColour(part);
    const Colour body = tint.isTransparent() ? base : base.interpolatedWith(tint, 0.8f);

    // Contact shadow on the substrate.
    g.saveState();
    g.addTransform(planeTransform(0.0));
    g.setColour(juce::Colours::black.withAlpha(0.28f * alpha));
    const float off = h > 0.01 ? 0.8f + static_cast<float>(h) * 0.12f : 0.5f;
    if (isRound(part.kind)) g.fillEllipse(static_cast<float>(part.x) + off, static_cast<float>(part.y) + off, static_cast<float>(part.w), static_cast<float>(part.h));
    else g.fillRoundedRectangle(static_cast<float>(part.x) + off, static_cast<float>(part.y) + off, static_cast<float>(part.w), static_cast<float>(part.h), 0.8f);
    g.restoreState();

    // Side faces (extruded view).
    if (h > 0.01) {
        const double cx = part.x + part.w * 0.5, cy = part.y + part.h * 0.5;
        auto face = [&](double x0, double y0, double x1, double y1, double nx, double ny) {
            if (proj_.faceFacing(nx, ny) <= 0.02) return;
            juce::Path p;
            p.startNewSubPath(proj(x0, y0, 0)); p.lineTo(proj(x1, y1, 0)); p.lineTo(proj(x1, y1, h)); p.lineTo(proj(x0, y0, h)); p.closeSubPath();
            const float shade = std::clamp(0.62f + 0.32f * static_cast<float>(-nx * 0.5 + ny * 0.7), 0.35f, 1.0f);
            g.setColour((part.kind == Kind::Tube ? body.withAlpha(0.55f) : body).withMultipliedBrightness(shade).withAlpha(alpha * (part.kind == Kind::Tube ? 0.55f : 1.0f)));
            g.fillPath(p);
            g.setColour(juce::Colours::black.withAlpha(0.25f * alpha));
            g.strokePath(p, juce::PathStrokeType(0.6f));
        };
        if (isRound(part.kind)) {
            const int n = 28;
            const double rx = part.w * 0.5, ry = part.h * 0.5;
            for (int i = 0; i < n; ++i) {
                const double a0 = 2 * kPi * i / n, a1 = 2 * kPi * (i + 1) / n, am = 0.5 * (a0 + a1);
                face(cx + rx * std::cos(a0), cy + ry * std::sin(a0), cx + rx * std::cos(a1), cy + ry * std::sin(a1), std::cos(am), std::sin(am));
            }
        } else {
            face(part.x, part.y, part.x + part.w, part.y, 0, -1);
            face(part.x + part.w, part.y, part.x + part.w, part.y + part.h, 1, 0);
            face(part.x + part.w, part.y + part.h, part.x, part.y + part.h, 0, 1);
            face(part.x, part.y + part.h, part.x, part.y, -1, 0);
        }
    }
    drawTopArt(g, part, body, alpha);

    if (hatch) {  // no voltage telemetry for this node: hatch instead of inventing a colour
        g.saveState();
        g.addTransform(planeTransform(h));
        juce::Path clip;
        if (isRound(part.kind)) clip.addEllipse(static_cast<float>(part.x), static_cast<float>(part.y), static_cast<float>(part.w), static_cast<float>(part.h));
        else clip.addRectangle(static_cast<float>(part.x), static_cast<float>(part.y), static_cast<float>(part.w), static_cast<float>(part.h));
        g.reduceClipRegion(clip);
        g.setColour(t.text.withAlpha(0.55f));
        for (float k = -static_cast<float>(part.h); k < static_cast<float>(part.w + part.h); k += 2.0f)
            g.drawLine(static_cast<float>(part.x) + k, static_cast<float>(part.y), static_cast<float>(part.x) + k - static_cast<float>(part.h), static_cast<float>(part.y + part.h), 0.3f);
        g.restoreState();
    }
    if (xray_) {  // ghosted bodies get a crisp outline so they remain legible
        g.saveState();
        g.addTransform(planeTransform(h));
        g.setColour(t.text.withAlpha(0.8f));
        if (isRound(part.kind)) g.drawEllipse(static_cast<float>(part.x), static_cast<float>(part.y), static_cast<float>(part.w), static_cast<float>(part.h), 0.4f);
        else g.drawRoundedRectangle(static_cast<float>(part.x), static_cast<float>(part.y), static_cast<float>(part.w), static_cast<float>(part.h), 0.8f, 0.4f);
        g.restoreState();
    }
}

void PcbCanvas::drawTopArt(Graphics& g, const pcb::Part& part, Colour body, float alpha) {
    const double h = part.height * heightScale();
    g.saveState();
    g.addTransform(planeTransform(h));
    const float x = static_cast<float>(part.x), y = static_cast<float>(part.y), w = static_cast<float>(part.w), hh = static_cast<float>(part.h);
    const float cx = x + w * 0.5f, cy = y + hh * 0.5f;
    const bool horiz = w >= hh;
    const Colour lead = Colour(0xffc9ced4).withAlpha(alpha);
    const auto& rd = readouts_[static_cast<std::size_t>(part.id)];
    switch (part.kind) {
        case Kind::Resistor: {
            const float L = horiz ? w : hh, lead_len = 1.7f;
            g.setColour(lead);
            if (horiz) g.drawLine(x, cy, x + w, cy, 0.7f); else g.drawLine(cx, y, cx, y + hh, 0.7f);
            const auto bodyR = horiz ? Rectangle<float>(x + lead_len, y, w - 2 * lead_len, hh) : Rectangle<float>(x, y + lead_len, w, hh - 2 * lead_len);
            g.setGradientFill(juce::ColourGradient(body.brighter(0.25f).withAlpha(alpha), horiz ? Point<float>(cx, y) : Point<float>(x, cy),
                                                    body.darker(0.3f).withAlpha(alpha), horiz ? Point<float>(cx, y + hh) : Point<float>(x + w, cy), false));
            g.fillRoundedRectangle(bodyR, 0.9f);
            const Colour bands[4] = {Colour(0xff7b4a1e), Colour(0xff111111), Colour(0xffb02020), Colour(0xffd4af37)};
            const float fr[4] = {0.18f, 0.32f, 0.46f, 0.80f};
            for (int i = 0; i < 4; ++i) {
                g.setColour(bands[i].withAlpha(alpha));
                if (horiz) g.fillRect(bodyR.getX() + bodyR.getWidth() * fr[i], bodyR.getY() + 0.1f, 0.75f, bodyR.getHeight() - 0.2f);
                else g.fillRect(bodyR.getX() + 0.1f, bodyR.getY() + bodyR.getHeight() * fr[i], bodyR.getWidth() - 0.2f, 0.75f);
            }
            (void) L;
            break;
        }
        case Kind::FilmCap: {
            g.setColour(lead);
            if (horiz) g.drawLine(x - 1.4f, cy, x + w + 1.4f, cy, 0.7f); else g.drawLine(cx, y - 1.4f, cx, y + hh + 1.4f, 0.7f);
            g.setGradientFill(juce::ColourGradient(body.brighter(0.3f).withAlpha(alpha), x, y, body.darker(0.25f).withAlpha(alpha), x + w, y + hh, false));
            g.fillRoundedRectangle(x, y, w, hh, 1.2f);
            g.setColour(juce::Colours::white.withAlpha(0.35f * alpha));
            g.fillRoundedRectangle(x + 0.8f, y + 0.5f, w - 1.6f, 0.9f, 0.4f);
            g.setColour(juce::Colours::black.withAlpha(0.5f * alpha));
            g.setFont(fnt(1.9f, true));
            g.drawText("film", Rectangle<float>(x, cy - 1.0f, w, 2.0f), juce::Justification::centred);
            break;
        }
        case Kind::Electrolytic: {
            const float r = std::min(w, hh) * 0.5f;
            g.setGradientFill(juce::ColourGradient(body.brighter(0.4f).withAlpha(alpha), cx - r * 0.6f, cy - r * 0.6f, body.darker(0.5f).withAlpha(alpha), cx + r, cy + r, true));
            g.fillEllipse(cx - r, cy - r, 2 * r, 2 * r);
            g.setGradientFill(juce::ColourGradient(Colour(0xffe5e8ec).withAlpha(alpha), cx - r * 0.4f, cy - r * 0.4f, Colour(0xff8c939c).withAlpha(alpha), cx + r * 0.7f, cy + r * 0.7f, true));
            g.fillEllipse(cx - r * 0.84f, cy - r * 0.84f, r * 1.68f, r * 1.68f);
            g.setColour(Colour(0xff4b5058).withAlpha(0.8f * alpha));  // pressure-relief vent score
            g.drawLine(cx - r * 0.5f, cy, cx + r * 0.5f, cy, 0.35f);
            g.drawLine(cx, cy - r * 0.5f, cx, cy + r * 0.5f, 0.35f);
            g.setColour(Colour(0xffd8dde3).withAlpha(0.8f * alpha));  // polarity stripe
            g.fillRoundedRectangle(cx - r, cy - r * 0.35f, r * 0.2f, r * 0.7f, 0.3f);
            break;
        }
        case Kind::Tube: {
            const float r = std::min(w, hh) * 0.5f;
            const double pos = rd.hasTemp ? pcb::thermalPosition(part.ref, rd.tempC) : 0.3;
            const Colour glow = overlay_ == 1 && rd.hasTemp ? analysis::thermalColour(*theme_, pos) : Colour(0xffff9a3d);
            const float gi = (overlay_ == 1 ? 0.55f : 0.30f) + 0.35f * static_cast<float>(pos);
            // glow halo
            g.setGradientFill(juce::ColourGradient(glow.withAlpha(gi * alpha), cx, cy, glow.withAlpha(0.0f), cx + r * 1.7f, cy, true));
            g.fillEllipse(cx - r * 1.7f, cy - r * 1.7f, r * 3.4f, r * 3.4f);
            g.setColour(Colour(0xff0e1114).withAlpha(0.85f * alpha));
            g.fillEllipse(cx - r, cy - r, 2 * r, 2 * r);
            g.setGradientFill(juce::ColourGradient(body.brighter(0.3f).withAlpha(0.42f * alpha), cx - r * 0.5f, cy - r * 0.6f, body.darker(0.3f).withAlpha(0.22f * alpha), cx + r, cy + r, true));
            g.fillEllipse(cx - r, cy - r, 2 * r, 2 * r);
            g.setColour(Colour(0xffdce6ee).withAlpha(0.8f * alpha));
            g.drawEllipse(cx - r, cy - r, 2 * r, 2 * r, 0.5f);
            // electrode structure: plate (outer), grid (dashed), cathode glow (inner)
            g.setColour(Colour(0xff9aa3ad).withAlpha(alpha));
            g.drawRoundedRectangle(cx - r * 0.55f, cy - r * 0.7f, r * 1.1f, r * 1.4f, r * 0.3f, 0.45f);
            g.setColour(Colour(0xffc9ced4).withAlpha(alpha));
            for (int i = -2; i <= 2; ++i) g.drawLine(cx - r * 0.34f, cy + i * r * 0.22f, cx + r * 0.34f, cy + i * r * 0.22f, 0.25f);
            g.setColour(glow.brighter(0.3f).withAlpha(alpha));
            g.fillEllipse(cx - r * 0.12f, cy - r * 0.45f, r * 0.24f, r * 0.9f);
            g.setColour(Colour(0xffdce6ee).withAlpha(0.5f * alpha));
            g.drawEllipse(cx - r * 0.78f, cy - r * 0.9f, r * 0.5f, r * 0.35f, 0.3f);  // specular
            break;
        }
        case Kind::Transformer: {
            g.setColour(body.withAlpha(alpha));
            g.fillRoundedRectangle(x, y, w, hh, 0.9f);
            g.setColour(juce::Colours::black.withAlpha(0.28f * alpha));
            for (float yy = y + 0.8f; yy < y + hh; yy += 1.1f) g.drawLine(x + 0.3f, yy, x + w - 0.3f, yy, 0.22f);
            const Colour cu = theme_->pcbCopper.withAlpha(alpha);
            for (int i = 0; i < 2; ++i) {
                const float bx = i == 0 ? x + 1.6f : cx + 0.8f, bw = w * 0.5f - 2.4f;
                g.setGradientFill(juce::ColourGradient(cu.brighter(0.35f), bx, y, cu.darker(0.35f), bx + bw, y, false));
                g.fillRoundedRectangle(bx, y + 3.0f, bw, hh - 6.0f, 1.0f);
                g.setColour(juce::Colours::black.withAlpha(0.3f * alpha));
                for (float yy = y + 3.6f; yy < y + hh - 3.0f; yy += 0.9f) g.drawLine(bx + 0.2f, yy, bx + bw - 0.2f, yy, 0.2f);
            }
            g.setColour(Colour(0xffc9ced4).withAlpha(alpha));
            for (float px : {x + 2.5f, cx - 1.0f, cx + 1.0f, x + w - 2.5f}) g.fillEllipse(px - 0.45f, y + hh - 1.2f, 0.9f, 0.9f);
            break;
        }
        case Kind::Diode: {
            g.setColour(lead);
            g.drawLine(x, cy, x + w, cy, 0.7f);
            const auto bodyR = Rectangle<float>(x + 2.2f, y, w - 4.4f, hh);
            g.setColour(body.withAlpha(alpha));
            g.fillRoundedRectangle(bodyR, 1.0f);
            g.setColour(Colour(0xffd8dde3).withAlpha(alpha));
            g.fillRect(bodyR.getRight() - 1.9f, bodyR.getY() + 0.1f, 0.9f, bodyR.getHeight() - 0.2f);
            g.setColour(juce::Colours::white.withAlpha(0.18f * alpha));
            g.fillRoundedRectangle(bodyR.getX() + 0.5f, bodyR.getY() + 0.4f, bodyR.getWidth() - 3.2f, 0.7f, 0.3f);
            break;
        }
        case Kind::Chassis: {
            g.setGradientFill(juce::ColourGradient(body.brighter(0.3f).withAlpha(alpha), x, y, body.darker(0.25f).withAlpha(alpha), x + w, y + hh, false));
            g.fillRoundedRectangle(x, y, w, hh, 1.2f);
            g.setColour(juce::Colours::white.withAlpha(0.12f * alpha));
            for (float k = 0; k < w + hh; k += 1.6f) g.drawLine(x + k, y, x + k - hh, y + hh, 0.25f);
            g.setColour(Colour(0xff3d434a).withAlpha(alpha));
            for (auto c : {Point<float>(x + 2.0f, y + 2.0f), Point<float>(x + w - 2.0f, y + 2.0f), Point<float>(x + 2.0f, y + hh - 2.0f), Point<float>(x + w - 2.0f, y + hh - 2.0f)})
                g.fillEllipse(c.x - 0.8f, c.y - 0.8f, 1.6f, 1.6f);
            g.setColour(theme_->text.withAlpha(0.6f * alpha));
            g.setFont(fnt(2.4f, true));
            g.drawText("CHASSIS", Rectangle<float>(x, cy - 1.0f, w, 2.0f), juce::Justification::centred);
            break;
        }
    }
    g.restoreState();
}

void PcbCanvas::drawSymbol(Graphics& g, const pcb::Part& part) {
    // Schematic symbol drawn in a local frame whose x axis runs along the part's long axis.
    const float cx = static_cast<float>(part.x + part.w * 0.5), cy = static_cast<float>(part.y + part.h * 0.5);
    const bool horiz = part.w >= part.h;
    const float L = static_cast<float>(horiz ? part.w : part.h);
    const float T = static_cast<float>(horiz ? part.h : part.w);
    auto xf = juce::AffineTransform::rotation(horiz ? 0.0f : static_cast<float>(kPi / 2)).translated(cx, cy).followedBy(planeTransform(0.0));
    g.saveState();
    g.addTransform(xf);
    const auto& th = *theme_;
    const bool sel = part.id == selected_;
    Colour ink = sel ? th.focus : th.text;
    bool hatch = false;
    const Colour tint = tintFor(part, hatch);
    if (!tint.isTransparent()) ink = tint.brighter(0.2f);
    g.setColour(ink);
    const float lw = 0.55f;
    juce::Path p;
    const float a = -L * 0.5f, b = L * 0.5f;
    switch (part.kind) {
        case Kind::Resistor: {
            p.startNewSubPath(a, 0);
            const float s = L / 12.0f;
            p.lineTo(a + 2 * s, 0);
            const float amp = std::min(T * 0.5f, 1.8f);
            for (int i = 0; i < 6; ++i) p.lineTo(a + 2 * s + (i + 0.5f) * (L - 4 * s) / 6.0f, (i % 2 == 0 ? -amp : amp));
            p.lineTo(b - 2 * s, 0); p.lineTo(b, 0);
            break;
        }
        case Kind::FilmCap: case Kind::Electrolytic: {
            const float pl = std::max(T, 4.0f) * 0.5f;
            p.startNewSubPath(a, 0); p.lineTo(-0.9f, 0);
            p.startNewSubPath(-0.9f, -pl); p.lineTo(-0.9f, pl);
            if (part.kind == Kind::Electrolytic) { p.startNewSubPath(1.6f, -pl); p.quadraticTo(0.2f, 0, 1.6f, pl); }
            else { p.startNewSubPath(0.9f, -pl); p.lineTo(0.9f, pl); }
            p.startNewSubPath(part.kind == Kind::Electrolytic ? 1.1f : 0.9f, 0); p.lineTo(b, 0);
            break;
        }
        case Kind::Tube: {
            const float r = std::min(L, T) * 0.5f - 0.5f;
            p.addEllipse(-r, -r, 2 * r, 2 * r);
            p.startNewSubPath(-r * 0.5f, -r * 0.45f); p.lineTo(r * 0.5f, -r * 0.45f);            // plate
            for (float k = -0.5f; k <= 0.51f; k += 0.33f) { p.startNewSubPath(r * k - 0.45f, 0); p.lineTo(r * k + 0.45f, 0); }  // grid
            p.startNewSubPath(-r * 0.45f, r * 0.5f); p.lineTo(r * 0.45f, r * 0.5f);              // cathode
            p.startNewSubPath(0, -r); p.lineTo(0, -r * 0.45f);
            break;
        }
        case Kind::Transformer: {
            const float r = L * 0.12f;
            for (int side = -1; side <= 1; side += 2)
                for (int i = 0; i < 4; ++i) p.addCentredArc(side * L * 0.28f, -T * 0.3f + i * r * 1.6f, r, r, 0, side < 0 ? kPi : 0.0, side < 0 ? 2 * kPi : kPi, i == 0);
            p.startNewSubPath(-0.7f, -T * 0.4f); p.lineTo(-0.7f, T * 0.4f);
            p.startNewSubPath(0.7f, -T * 0.4f); p.lineTo(0.7f, T * 0.4f);
            break;
        }
        case Kind::Diode: {
            p.startNewSubPath(a, 0); p.lineTo(-1.6f, 0);
            p.startNewSubPath(-1.6f, -2.0f); p.lineTo(-1.6f, 2.0f); p.lineTo(1.6f, 0); p.closeSubPath();
            p.startNewSubPath(1.6f, -2.0f); p.lineTo(1.6f, 2.0f);
            p.startNewSubPath(1.6f, 0); p.lineTo(b, 0);
            break;
        }
        case Kind::Chassis: {
            p.addRoundedRectangle(a, -T * 0.5f, L, T, 1.0f);
            for (int i = 0; i < 3; ++i) { p.startNewSubPath(-3.0f + i * 1.5f, -1.0f + i * 1.6f - 1.0f); p.lineTo(3.0f - i * 1.5f, -1.0f + i * 1.6f - 1.0f); }
            break;
        }
    }
    g.strokePath(p, juce::PathStrokeType(lw, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.restoreState();
}

void PcbCanvas::drawFlows(Graphics& g) {
    if (overlay_ != 3 || !(valid_ || frame_.generation != 0)) return;
    const auto& t = *theme_;
    const auto& st = frame_.slot[slot_];
    const double fs = frame_.sampleRate > 0 ? frame_.sampleRate : 48000.0;
    const float spacing = 15.0f;
    const float phase = (reduceMotion_ || stale_) ? 0.0f : static_cast<float>(std::fmod(static_cast<double>(frame_.sampleIndex) / fs * 36.0, spacing));
    for (const auto& fp : pcb::flowPaths()) {
        double mag = 0;
        juce::String text;
        switch (fp.source) {
            case pcb::FlowSource::InputLevel: mag = frame_.inPeak[channel_]; text = juce::String(fp.label) + " " + juce::String(mag, 2) + " FS pk"; break;
            case pcb::FlowSource::OutputLevel: mag = frame_.outPeak[channel_]; text = juce::String(fp.label) + " " + juce::String(mag, 2) + " FS pk"; break;
            case pcb::FlowSource::StageCurrent: mag = std::abs(st.stageCurrentA[channel_]) / runMaxCurrent_; text = juce::String(fp.label) + " " + formatSi(st.stageCurrentA[channel_], "A"); break;
        }
        const float rel = static_cast<float>(std::clamp(mag, 0.0, 1.0));
        const float a = smooth(0.02f, 0.22f, rel) * (stale_ ? 0.4f : 1.0f);  // fade near zero instead of implying flow
        std::vector<Point<float>> pts;
        for (const auto& p : fp.pts) pts.push_back(proj(p.x, p.y, 0.4 + 0.4 * heightScale()));
        float total = 0;
        for (std::size_t i = 1; i < pts.size(); ++i) total += pts[i].getDistanceFrom(pts[i - 1]);
        juce::Path under;
        for (std::size_t i = 0; i < pts.size(); ++i) { if (i == 0) under.startNewSubPath(pts[i]); else under.lineTo(pts[i]); }
        g.setColour(t.signal.withAlpha(0.28f * a));
        g.strokePath(under, juce::PathStrokeType(1.5f + 3.5f * rel, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        if (a > 0.04f) {
            g.setColour(t.signal.withAlpha(std::min(1.0f, 0.35f + a)));
            const float sz = 3.0f + 3.5f * rel;
            for (float pos = phase; pos < total; pos += spacing) {
                float acc = 0;
                for (std::size_t i = 1; i < pts.size(); ++i) {
                    const float seg = pts[i].getDistanceFrom(pts[i - 1]);
                    if (acc + seg >= pos && seg > 0.01f) {
                        const float u = (pos - acc) / seg;
                        const Point<float> c = pts[i - 1] + (pts[i] - pts[i - 1]) * u;
                        const Point<float> d = (pts[i] - pts[i - 1]) / seg;
                        const Point<float> n(-d.y, d.x);
                        juce::Path chev;
                        chev.startNewSubPath(c - d * sz + n * sz * 0.8f); chev.lineTo(c + d * sz * 0.4f); chev.lineTo(c - d * sz - n * sz * 0.8f);
                        g.strokePath(chev, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                        break;
                    }
                    acc += seg;
                }
            }
        }
        // numeric label at the path middle (never colour alone)
        if (!pts.empty()) {
            const auto mid = pts[pts.size() / 2];
            g.setFont(fnt(10.0f, true));
            const float w = strW(g.getCurrentFont(), text + (a < 0.05f ? juce::String(" ~0") : juce::String())) + 10.0f;
            const auto r = Rectangle<float>(mid.x - w * 0.5f, mid.y + 6.0f, w, 14.0f).constrainedWithin(getLocalBounds().toFloat().reduced(2.0f));
            g.setColour(t.panel.withAlpha(0.85f));
            g.fillRoundedRectangle(r, 3.0f);
            g.setColour(a < 0.05f ? t.textDim : t.signal);
            g.drawText(text + (a < 0.05f ? " ~0" : ""), r, juce::Justification::centred);
        }
    }
}

void PcbCanvas::drawLabels(Graphics& g) {
    const auto& t = *theme_;
    std::vector<Rectangle<float>> placed, partRects;
    const auto bounds = Rectangle<float>(2.0f, 2.0f, static_cast<float>(getWidth()) - 4.0f, static_cast<float>(getHeight() - kLegendH) - 4.0f);
    for (const auto& part : pcb::parts()) {
        const auto b = pcb::projectedBounds(proj_, part, heightScale(), 0);
        partRects.emplace_back(static_cast<float>(b.x0), static_cast<float>(b.y0), static_cast<float>(b.x1 - b.x0), static_cast<float>(b.y1 - b.y0));
    }
    std::vector<int> order;
    if (selected_ >= 0) order.push_back(selected_);
    for (int i = 0; i < pcb::kPartCount; ++i) if (i != selected_) order.push_back(i);
    for (int id : order) {
        const auto& part = pcb::parts()[static_cast<std::size_t>(id)];
        const auto& r = readouts_[static_cast<std::size_t>(id)];
        juce::String value;
        if (overlay_ == 1 && r.hasTemp) value = juce::String(r.tempC, 1) + degC();
        else if (overlay_ == 2) value = r.hasVoltage ? r.voltageText : juce::String("n/a");
        else if (xray_) {
            for (const auto& row : r.values) if (row.label == "Effective" || row.label.startsWith("Material")) { value = row.value.upToFirstOccurrenceOf(" (", false, false); break; }
        }
        const float markerW = (overlay_ == 1 && r.hasTemp) ? 10.0f : 0.0f;
        const auto kf = fnt(10.5f, true), vf = fnt(10.0f);
        const float w1 = strW(kf, part.key);
        const float w2 = value.isEmpty() ? 0.0f : strW(vf, value) + markerW + 5.0f;
        const float w = w1 + w2 + 8.0f, h = 14.0f;
        const auto pb = partRects[static_cast<std::size_t>(id)];
        const float cx = pb.getCentreX(), cy = pb.getCentreY();
        const Rectangle<float> cands[10] = {
            {cx - w * 0.5f, pb.getY() - h - 3, w, h}, {cx - w * 0.5f, pb.getBottom() + 3, w, h},
            {pb.getRight() + 4, cy - h * 0.5f, w, h}, {pb.getX() - w - 4, cy - h * 0.5f, w, h},
            {cx - w * 0.5f, pb.getY() - 2 * h - 5, w, h}, {cx - w * 0.5f, pb.getBottom() + h + 5, w, h},
            {pb.getRight() + 3, pb.getY() - h - 2, w, h}, {pb.getX() - w - 3, pb.getY() - h - 2, w, h},
            {pb.getRight() + 3, pb.getBottom() + 2, w, h}, {pb.getX() - w - 3, pb.getBottom() + 2, w, h}};
        Rectangle<float> pick = cands[0].constrainedWithin(bounds);
        float best = 1e9f;
        for (const auto& c : cands) {
            const auto cc = c.constrainedWithin(bounds);
            float score = std::abs(cc.getX() - c.getX()) + std::abs(cc.getY() - c.getY());  // pushed by the viewport edge
            for (const auto& p : placed) if (p.expanded(1.5f).intersects(cc)) score += 100.0f;
            for (std::size_t i = 0; i < partRects.size(); ++i) if (static_cast<int>(i) != id && partRects[i].intersects(cc)) score += 30.0f;
            if (score < best) { best = score; pick = cc; if (score <= 0.01f) break; }
        }
        placed.push_back(pick);
        const bool sel = id == selected_;
        g.setColour(t.panel.withAlpha(0.9f));
        g.fillRoundedRectangle(pick, 3.0f);
        g.setColour(sel ? t.focus : t.border.withAlpha(0.7f));
        g.drawRoundedRectangle(pick, 3.0f, sel ? 1.8f : 0.8f);
        g.setFont(kf);
        g.setColour(t.text);
        g.drawText(part.key, Rectangle<float>(pick.getX() + 4.0f, pick.getY(), w1 + 2.0f, h), juce::Justification::centredLeft);
        if (r.fault) {  // fault badge (shape + text, not colour only)
            g.setColour(t.danger);
            juce::Path tri;
            const float bx = pick.getRight() - 1.0f, by = pick.getY() - 1.0f;
            tri.addTriangle(bx - 6.0f, by + 9.0f, bx + 6.0f, by + 9.0f, bx, by - 3.0f);
            g.fillPath(tri);
            g.setColour(t.textInverse);
            g.setFont(fnt(9.0f, true));
            g.drawText("!", Rectangle<float>(bx - 4.0f, by + 1.0f, 8.0f, 9.0f), juce::Justification::centred);
        }
        if (value.isNotEmpty()) {
            auto row = Rectangle<float>(pick.getX() + 4.0f + w1 + 5.0f, pick.getY(), w2, h);
            if (markerW > 0) {
                const Colour mc = analysis::thermalColour(t, pcb::thermalPosition(part.ref, r.tempC));
                const float mx = row.getX() + 4.0f, my = row.getCentreY();
                juce::Path m;
                switch (r.state) {
                    case pcb::ThermalState::Cool: m.addTriangle(mx - 3.0f, my - 2.0f, mx + 3.0f, my - 2.0f, mx, my + 3.0f); break;     // down: cool
                    case pcb::ThermalState::Normal: m.addEllipse(mx - 2.6f, my - 2.6f, 5.2f, 5.2f); break;                                // dot: normal
                    case pcb::ThermalState::Warm: m.addTriangle(mx - 3.0f, my + 3.0f, mx + 3.0f, my + 3.0f, mx, my - 3.0f); break;     // up: warm
                    case pcb::ThermalState::Stress: m.addStar({mx, my}, 4, 1.4f, 3.6f); break;                                             // star: stress
                }
                g.setColour(mc);
                g.fillPath(m);
                g.setColour(t.border);
                g.strokePath(m, juce::PathStrokeType(0.7f));
                row.removeFromLeft(markerW);
            }
            g.setFont(vf);
            g.setColour(stale_ ? t.textDim : t.text);
            g.drawText(value, row, juce::Justification::centredLeft);
        }
    }
}

void PcbCanvas::drawSelection(Graphics& g) {
    const auto& t = *theme_;
    auto outline = [&](int id, Colour c, float w, bool dashed) {
        if (id < 0) return;
        const auto b = pcb::projectedBounds(proj_, pcb::parts()[static_cast<std::size_t>(id)], heightScale(), 0);
        const auto r = Rectangle<float>(static_cast<float>(b.x0), static_cast<float>(b.y0), static_cast<float>(b.x1 - b.x0), static_cast<float>(b.y1 - b.y0)).expanded(4.0f);
        g.setColour(c);
        if (dashed) {
            juce::Path p, d;
            p.addRoundedRectangle(r, 4.0f);
            const float dashes[2] = {4.0f, 3.0f};
            juce::PathStrokeType(w).createDashedStroke(d, p, dashes, 2);
            g.fillPath(d);
        } else {
            g.drawRoundedRectangle(r, 4.0f, w);
        }
    };
    if (hovered_ >= 0 && hovered_ != selected_) outline(hovered_, t.text.withAlpha(0.6f), 1.0f, true);
    outline(selected_, t.focus, t.focusOutline, false);
    for (int id : brushSet_) if (id != selected_) outline(id, t.accent, 1.6f, true);
}

void PcbCanvas::drawLegend(Graphics& g) {
    // Dedicated strip below the board so the legend never hides components.
    const auto& t = *theme_;
    const auto strip = Rectangle<float>(0.0f, static_cast<float>(getHeight() - kLegendH), static_cast<float>(getWidth()), static_cast<float>(kLegendH));
    g.setColour(t.panel);
    g.fillRect(strip);
    g.setColour(t.border.withAlpha(0.6f));
    g.drawLine(strip.getX(), strip.getY(), strip.getRight(), strip.getY(), 1.0f);
    const float x = 10.0f, y = strip.getY();
    const auto bar = Rectangle<float>(x, y + 6.0f, 150.0f, 7.0f);
    auto caption = [&](const juce::String& text) {
        g.setColour(t.textDim);
        g.setFont(fnt(10.0f));
        g.drawFittedText(text, Rectangle<float>(bar.getRight() + 14.0f, y + 2.0f, strip.getWidth() - bar.getRight() - 24.0f, static_cast<float>(kLegendH) - 4.0f).toNearestInt(),
                         juce::Justification::centredLeft, 2, 0.85f);
    };
    g.setFont(fnt(9.5f));
    if (overlay_ == 0) {
        g.setColour(t.textDim);
        g.setFont(fnt(10.0f));
        g.drawText("Overlay off. Synthetic fixture (G01): illustrative board and references, not calibrated hardware.",
                   strip.reduced(10.0f, 0.0f), juce::Justification::centredLeft, true);
    } else if (overlay_ == 1) {
        juce::ColourGradient grad(t.thermalCold, bar.getX(), 0, t.thermalHot, bar.getRight(), 0, false);
        grad.addColour(0.5, t.thermalNormal);
        g.setGradientFill(grad);
        g.fillRect(bar);
        g.setColour(t.text);
        g.drawText("cool", Rectangle<float>(bar.getX(), y + 14.0f, 40, 11), juce::Justification::centredLeft);
        g.drawText("normal", Rectangle<float>(bar.getCentreX() - 25.0f, y + 14.0f, 50, 11), juce::Justification::centred);
        g.drawText("stress", Rectangle<float>(bar.getRight() - 40.0f, y + 14.0f, 40, 11), juce::Justification::centredRight);
        caption("Thermal: per-component reference temperatures (illustrative, G01; see inspector). Marker shape: down-triangle cool, dot normal, up-triangle warm, star stress.");
    } else if (overlay_ == 2) {
        juce::ColourGradient grad(t.voltageNeg, bar.getX(), 0, t.voltagePos, bar.getRight(), 0, false);
        grad.addColour(0.5, t.panelRaised);
        g.setGradientFill(grad);
        g.fillRect(bar);
        const double vn = valid_ || frame_.generation != 0 ? frame_.slot[slot_].railNominalVolts : 0.0;
        const juce::String vs = vn > 0 ? juce::String(vn, 0) + " V" : juce::String("V_nom");
        g.setColour(t.text);
        g.drawText("-" + vs, Rectangle<float>(bar.getX(), y + 14.0f, 60, 11), juce::Justification::centredLeft);
        g.drawText("0", Rectangle<float>(bar.getCentreX() - 10.0f, y + 14.0f, 20, 11), juce::Justification::centred);
        g.drawText("+" + vs, Rectangle<float>(bar.getRight() - 60.0f, y + 14.0f, 60, 11), juce::Justification::centredRight);
        caption("Voltage: signed rail scale (declared +-V_nom, reservoir V_res). Audio nodes show FS peaks. Hatched = no voltage telemetry for that node.");
    } else {
        g.setColour(t.signal);
        juce::Path chev;
        for (float cx = bar.getX() + 10.0f; cx < bar.getRight(); cx += 22.0f) { chev.startNewSubPath(cx - 3.0f, bar.getY() - 2.0f); chev.lineTo(cx + 2.0f, bar.getCentreY()); chev.lineTo(cx - 3.0f, bar.getBottom() + 2.0f); }
        g.strokePath(chev, juce::PathStrokeType(1.5f));
        g.setColour(t.text);
        g.drawText("direction of simulated flow", Rectangle<float>(bar.getX(), y + 14.0f, bar.getWidth(), 11), juce::Justification::centredLeft);
        caption("Signal: chevron size and brightness = relative magnitude (audio peaks FS, plate current vs its recent maximum); fades near zero (~0).");
    }
}

}  // namespace cd::plugin::ui
