// Workbench snapshot + contract test (CD-F17, UX-01, UX-04, UX-05, UX-06, UX-10, T17 UI parts).
// Renders the Workbench against FakeBridge (SYNTHETIC telemetry, labelled as such) to PNG for every
// overlay, X-Ray, the oblique projection, the schematic fallback, both themes and scales 100/150/200,
// and checks the analysis math and gesture contracts. Synthetic fixtures only: no physical validation.
#include <cstdio>
#include <set>
#include "FakeBridge.h"
#include "ui/ProcessorEditorBridge.h"
#include "ui/analysis/AnalysisDisplays.h"
#include "ui/analysis/AnalysisMath.h"
#include "ui/workbench/GestureMath.h"
#include "ui/workbench/InspectorPanel.h"
#include "ui/workbench/PcbCanvas.h"
#include "ui/workbench/PcbModel.h"
#include "ui/workbench/WorkbenchView.h"

namespace analysis = cd::plugin::ui::analysis;
namespace gesture = cd::plugin::ui::gesture;
namespace pcb = cd::plugin::ui::pcb;
using cd::plugin::test::FakeBridge;
using cd::plugin::ui::InspectorPanel;
using cd::plugin::ui::PcbCanvas;
using cd::plugin::ui::WorkbenchView;

static int failures = 0;
static void check(bool ok, const juce::String& what) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what.toRawUTF8()); }
}
static bool near(double a, double b, double tol) { return std::abs(a - b) <= tol; }

// Deterministic white noise in [-1, 1] by sample index (hash), so tests are reproducible.
static double noiseAt(std::uint64_t n, std::uint64_t seed) {
    std::uint64_t x = n * 0x9E3779B97F4A7C15ull + seed * 0xD1B54A32D192ED03ull;
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 27; x *= 0x94D049BB133111EBull; x ^= x >> 31;
    return static_cast<double>(x >> 11) / static_cast<double>(1ull << 52) - 1.0;
}

// ---- math contracts -----------------------------------------------------------------------------------
static void testAnalysisMath() {
    // UX-04: unity XY diagonal <= 0.5 display px on a non-square plot (equal scales), 0.5 gain slope within 1%.
    {
        const auto m = analysis::makeEqualScaleMap(10, 20, 300, 200, 1.0);
        std::vector<double> in, out, half;
        for (int i = 0; i <= 256; ++i) { const double v = -1.0 + 2.0 * i / 256.0; in.push_back(v); out.push_back(v); half.push_back(0.5 * v); }
        const double err = analysis::maxPixelErrorToReference(m, in.data(), out.data(), static_cast<int>(in.size()), 1.0, 1.0);
        check(err <= 0.5, "unity diagonal pixel error " + juce::String(err, 6) + " <= 0.5 px");
        double x0, y0, x1, y1;
        analysis::referenceLine(m, 1.0, 1.0, x0, y0, x1, y1);
        check(near(std::abs(x1 - x0), std::abs(y1 - y0), 1e-9), "unity reference line is 45 degrees on screen");
        const double slope = analysis::pixelSlope(m, in.data(), half.data(), static_cast<int>(in.size()));
        check(near(slope, 0.5, 0.005), "0.5 gain pixel slope " + juce::String(slope, 5) + " within 1%");
        check(near(analysis::smallSignalSlope(in.data(), half.data(), static_cast<int>(in.size()), 0.3), 0.5, 0.005), "0.5 gain small-signal slope");
        // Ordered extrema decimation keeps the spike and stays within 2048 points, in time order.
        std::vector<double> x(10000), y(10000);
        for (int i = 0; i < 10000; ++i) { x[static_cast<std::size_t>(i)] = i; y[static_cast<std::size_t>(i)] = std::sin(i * 0.01); }
        y[4321] = 7.0;
        std::vector<double> ox, oy;
        analysis::decimateOrderedExtrema(x.data(), y.data(), 10000, 2048, ox, oy);
        check(ox.size() <= 2048 && !ox.empty(), "decimation bounded to 2048 points");
        check(std::find(oy.begin(), oy.end(), 7.0) != oy.end(), "decimation preserves the extremum");
        check(std::is_sorted(ox.begin(), ox.end()), "decimation keeps time order");
    }
    // UX-06: known delay phase within 3 degrees in coherent bins; independent noise has low coherence.
    {
        const double fs = 48000;
        const int delay = 3, n = static_cast<int>(fs * 4);
        std::vector<double> l(static_cast<std::size_t>(n)), r(static_cast<std::size_t>(n)), q(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            l[static_cast<std::size_t>(i)] = 0.3 * noiseAt(static_cast<std::uint64_t>(i), 1);
            q[static_cast<std::size_t>(i)] = 0.3 * noiseAt(static_cast<std::uint64_t>(i), 2);
        }
        for (int i = 0; i < n; ++i) r[static_cast<std::size_t>(i)] = i >= delay ? l[static_cast<std::size_t>(i - delay)] : 0.0;
        analysis::CrossSpectrumAnalyzer a(2048, 512, fs);
        a.push(l.data(), r.data(), n);
        std::vector<analysis::PhaseBin> bins;
        a.bins(bins);
        int coherentBins = 0;
        double worst = 0;
        for (const auto& b : bins) {
            if (b.freqHz < 200 || b.freqHz > 6000 || b.coherence < 0.9 || b.levelDbfs < -70) continue;
            const double expected = analysis::wrapPi(2.0 * analysis::kPi * b.freqHz * delay / fs);
            worst = std::max(worst, std::abs(analysis::wrapPi(b.phaseRad - expected)) * 180.0 / analysis::kPi);
            ++coherentBins;
        }
        check(coherentBins > 100, "delay test has coherent bins: " + juce::String(coherentBins));
        check(worst <= 3.0, "known-delay phase error " + juce::String(worst, 3) + " deg <= 3");
        // Independent noise -> low coherence.
        analysis::CrossSpectrumAnalyzer c(2048, 512, fs);
        c.push(l.data(), q.data(), n);
        c.bins(bins);
        double mean = 0;
        for (const auto& b : bins) mean += b.coherence;
        mean /= static_cast<double>(bins.size());
        check(mean < 0.2, "independent noise mean coherence " + juce::String(mean, 3) + " < 0.2");
        // Ring-limited frame mode (256-pt window, as the telemetry rings provide) also tracks the delay.
        analysis::CrossSpectrumAnalyzer f(256, 256, fs);
        double fw = 0;
        int cnt = 0;
        for (int frame = 0; frame < 120; ++frame) f.processFrame(l.data() + frame * 400, r.data() + frame * 400, 1.0 / 30.0);
        f.bins(bins);
        for (const auto& b : bins) {
            if (b.freqHz < 300 || b.freqHz > 6000 || b.coherence < 0.9 || b.levelDbfs < -70) continue;
            fw = std::max(fw, std::abs(analysis::wrapPi(b.phaseRad - analysis::wrapPi(2.0 * analysis::kPi * b.freqHz * delay / fs))) * 180.0 / analysis::kPi);
            ++cnt;
        }
        check(cnt > 20 && fw <= 3.0, "frame-mode phase error " + juce::String(fw, 3) + " deg over " + juce::String(cnt) + " bins");
        // Scatter mapping: log radius, phase angle, hide rule.
        const auto lo = analysis::scatterPoint(20.0, 0.0, 20000.0, 50, 50, 40);
        const auto hi = analysis::scatterPoint(20000.0, analysis::kPi / 2, 20000.0, 50, 50, 40);
        check(near(lo.radius01, 0.0, 1e-9) && near(hi.radius01, 1.0, 1e-9), "scatter radius 0 at 20 Hz, 1 at fmax");
        check(near(hi.x, 50, 1e-6) && near(hi.y, 10, 1e-6), "90 degree phase points up");
        analysis::PhaseBin quiet; quiet.levelDbfs = -80; quiet.coherence = 1.0;
        analysis::PhaseBin loud; loud.levelDbfs = -20; loud.coherence = 0.05;
        analysis::PhaseBin good; good.levelDbfs = -20; good.coherence = 0.5;
        check(!analysis::binVisible(quiet) && !analysis::binVisible(loud) && analysis::binVisible(good), "bins below -70 dBFS or coherence 0.1 hidden");
    }
    // UX-05: rail history, gap marking, 50 ms recovery within 2 ms, overlap/gap rejection.
    {
        const double fs = 48000, tau = 0.05 / std::log(20.0);
        auto railV = [&](double t) { return t < 1.0 ? 300.0 : 300.0 - 50.0 * std::exp(-(t - 1.0) / tau); };
        analysis::RailHistory h;
        for (int frame = 1; frame <= 90; ++frame) {  // 3 s at 30 Hz
            const std::int64_t si = static_cast<std::int64_t>(frame * fs / 30.0);
            double ring[cd::engine::kRingSize];
            for (int k = 0; k < cd::engine::kRingSize; ++k) ring[k] = railV(static_cast<double>(si - (cd::engine::kRingSize - 1 - k) * 48) / fs);
            h.ingest(si, fs, ring, 0, cd::engine::kRingSize, 48);
        }
        const auto t1 = h.newestBin() + 1;
        const auto r = analysis::measureRecovery(h, t1 - 2500, t1);
        check(r.status == analysis::RecoveryResult::Status::Recovered, "50 ms recovery detected");
        check(std::abs(r.recoveryMs - 50.0) <= 2.0, "recovery " + juce::String(r.recoveryMs, 1) + " ms within 2 ms of 50");
        check(near(r.minVolts, 250.0, 2.5) && near(r.preLevelVolts, 300.0, 0.1), "min rail and pre-event level");
        std::vector<analysis::EnvelopeColumn> env;
        analysis::decimateEnvelope(h, t1 - 2500, t1, 200, env);
        check(env.size() == 200 && env.front().valid && env.back().valid, "envelope columns valid without gaps");
        // A gap (dropped telemetry) is marked invalid and blocks a recovery measurement.
        analysis::RailHistory g2;
        for (int frame = 1; frame <= 90; ++frame) {
            if (frame >= 33 && frame <= 36) continue;  // ~130 ms hole just after the event
            const std::int64_t si = static_cast<std::int64_t>(frame * fs / 30.0);
            double ring[cd::engine::kRingSize];
            for (int k = 0; k < cd::engine::kRingSize; ++k) ring[k] = railV(static_cast<double>(si - (cd::engine::kRingSize - 1 - k) * 48) / fs);
            // 256 entries * 1 ms = 256 ms history per frame overlaps the hole, so shrink the ring to 24 ms for this test.
            double small[24];
            for (int k = 0; k < 24; ++k) small[k] = ring[cd::engine::kRingSize - 24 + k];
            g2.ingest(si, fs, small, 0, 24, 48);
        }
        const auto t2 = g2.newestBin() + 1;
        analysis::decimateEnvelope(g2, t2 - 2500, t2, 200, env);
        int invalid = 0;
        for (const auto& c : env) if (!c.valid) ++invalid;
        check(invalid > 0, "gaps are marked invalid in the envelope");
        const auto r2 = analysis::measureRecovery(g2, t2 - 2500, t2);
        check(r2.status == analysis::RecoveryResult::Status::RejectedGap || r2.status == analysis::RecoveryResult::Status::NoEvent,
              "recovery across a data gap is rejected, not reported");
        // Two overlapping droops are rejected.
        analysis::RailHistory g3;
        auto twoDips = [&](double t) { return t < 1.0 ? 300.0 : t < 1.02 ? 250.0 + 1250.0 * (t - 1.0) : t < 1.03 ? 255.0 : 300.0 - 45.0 * std::exp(-(t - 1.03) / 0.05); };
        for (int frame = 1; frame <= 90; ++frame) {
            const std::int64_t si = static_cast<std::int64_t>(frame * fs / 30.0);
            double ring[cd::engine::kRingSize];
            for (int k = 0; k < cd::engine::kRingSize; ++k) ring[k] = twoDips(static_cast<double>(si - (cd::engine::kRingSize - 1 - k) * 48) / fs);
            g3.ingest(si, fs, ring, 0, cd::engine::kRingSize, 48);
        }
        const auto t3 = g3.newestBin() + 1;
        const auto r3 = analysis::measureRecovery(g3, t3 - 2500, t3);
        check(r3.status == analysis::RecoveryResult::Status::RejectedOverlap, "overlapping events rejected");
    }
    // Stale detection (>250 ms), hold meters.
    {
        analysis::StaleTracker s;
        check(s.stale(0), "no frame yet -> stale");
        s.update(1, 100, true, 0.0);
        check(!s.stale(200.0), "fresh at 200 ms");
        check(s.stale(251.0), "stale after 250 ms without a new frame");
        s.update(1, 100, true, 300.0);
        check(s.stale(300.0 + 251.0), "an unchanged frame does not refresh staleness");
        s.update(2, 200, true, 320.0);
        check(!s.stale(330.0), "new frame clears stale");
        analysis::PeakHold p;
        p.update(0.9, 10); p.update(0.2, 500);
        check(near(p.held, 0.9, 1e-12), "peak held within 1 s");
        p.update(0.2, 600);
        check(near(p.held, 0.2, 1e-12), "peak hold released after 1 s");
        analysis::PeakHold c;
        c.update(1.0, 0);
        check(c.clipLit(), "clip lit on full-scale peak");
        c.update(0.1, 1900); check(c.clipLit(), "clip lamp holds 2 s");
        c.update(0.1, 200); check(!c.clipLit(), "clip lamp clears after 2 s");
    }
}

static void testGestureMath() {
    check(near(gesture::biasFromDrag(0.0, -200.0, false), 1.0, 1e-12), "200 px up spans the full normalized bias range");
    check(near(gesture::biasFromDrag(1.0, 200.0, false), 0.0, 1e-12), "200 px down spans the full range the other way");
    check(near(gesture::biasFromDrag(0.5, -100.0, false), 1.0, 1e-12), "100 px = half span");
    check(near(gesture::biasFromDrag(0.0, -200.0, true), 0.1, 1e-12), "Shift fine scale 0.1");
    check(near(gesture::biasFromDrag(0.9, -500.0, false), 1.0, 1e-12), "bias clamps at 1");
    check(near(gesture::supplyCapacitance(100.0, std::sqrt(2.0), 10, 1000), 200.0, 1e-9), "sqrt2 pinch doubles C");
    check(near(gesture::supplyCapacitance(100.0, 3.0, 10, 1000), 400.0, 1e-9), "pinch ratio clamps to 2 (C x4)");
    check(near(gesture::supplyCapacitance(400.0, 2.0, 10, 1000), 1000.0, 1e-9), "C clamps to the parameter maximum");
    check(near(gesture::supplyCapacitance(20.0, 0.5, 10, 1000), 10.0, 1e-9), "C clamps to the parameter minimum");
    check(near(gesture::sagContribution(std::sqrt(2.0)), -0.125, 1e-12), "sag contribution -log2(r)/4");
    check(near(gesture::ageFromPath(0.0, 400.0, false), 1.0, 1e-12) && near(gesture::ageFromPath(0.3, 400.0, true), 0.0, 1e-12), "age path/400, Alt reverses, clamped");
    check(near(gesture::keyAdjust(0.5, +1, false), 0.51, 1e-12) && near(gesture::keyAdjust(0.5, -1, true), 0.499, 1e-12), "arrows +-1% / Shift +-0.1%");
    const auto a = gesture::parseQuantity("470u", "F", 1e-6);
    const auto b = gesture::parseQuantity("0.47 mF", "F", 1e-6);
    const auto c = gesture::parseQuantity("100", "F", 1e-6);
    const auto d = gesture::parseQuantity("47 uF", "F", 1e-6);
    const auto e = gesture::parseQuantity("1e-4 F", "F", 1e-6);
    check(a.ok && near(a.value, 470e-6, 1e-12) && b.ok && near(b.value, 470e-6, 1e-12) && c.ok && near(c.value, 100e-6, 1e-12) &&
          d.ok && near(d.value, 47e-6, 1e-12) && e.ok && near(e.value, 1e-4, 1e-12), "numeric entry parses units");
    check(!gesture::parseQuantity("470 xyz", "F", 1e-6).ok && !gesture::parseQuantity("", "F").ok && !gesture::parseQuantity("abc", "F").ok, "numeric entry rejects garbage");
}

// ---- synthetic telemetry (labelled SYNTHETIC) ---------------------------------------------------------------
static double railModel(double t) {
    // Synthetic kick pattern: every 0.5 s the DC rail droops 40 V and recovers in ~50 ms (95%).
    const double tau = 0.05 / std::log(20.0), ph = std::fmod(t, 0.5);
    return 300.0 - 40.0 * std::exp(-ph / tau) - 2.0;
}

static cd::engine::TelemetryFrame richFrame(FakeBridge& bridge, double t, bool withMagnetics = true) {
    bridge.advance(t);
    cd::engine::TelemetryFrame f;
    bridge.readTelemetry(f);
    const double fs = 48000;
    for (int i = 0; i < cd::engine::kRingSize; ++i) {
        const std::int64_t n = f.sampleIndex - (cd::engine::kRingSize - 1 - i);
        const double dry = 0.10 * noiseAt(static_cast<std::uint64_t>(n), 11) + 0.75 * std::sin(2.0 * analysis::kPi * 700.0 * static_cast<double>(n) / fs);
        const std::int64_t nd = n - 2;
        const double dryDelayed = 0.10 * noiseAt(static_cast<std::uint64_t>(nd), 11) + 0.75 * std::sin(2.0 * analysis::kPi * 700.0 * static_cast<double>(nd) / fs);
        f.transferRing[i][0] = dry;
        f.transferRing[i][1] = 0.8 * std::tanh(1.6 * dryDelayed);
    }
    f.transferWrite = 0;
    f.ringWrite = 0;
    f.ringDecimation = 48;
    for (int s = 0; s < cd::engine::kSlots; ++s) {
        auto& st = f.slot[s];
        for (int k = 0; k < cd::engine::kRingSize; ++k)
            st.railRing[k] = railModel(static_cast<double>(f.sampleIndex - (cd::engine::kRingSize - 1 - k) * 48) / fs) - 3.0 * s;
        st.railVolts = railModel(t);
        st.railDcVolts = railModel(t) + 6.0;
        st.railNominalVolts = 300;
        st.family = 0;
        for (int ch = 0; ch < cd::engine::kMaxChannels; ++ch) {
            st.stageCurrentA[ch] = 1.2e-3 * (0.5 + 0.5 * std::sin(t * 9.0));
            if (!withMagnetics) {
                st.fluxWb[ch] = st.fieldH[ch] = st.fluxDensityT[ch] = 0;
            }
        }
        if (!withMagnetics) for (auto& p : st.bhRing) p[0] = p[1] = 0;
    }
    f.inPeak[0] = 0.62; f.inPeak[1] = 0.58; f.inRms[0] = 0.28; f.inRms[1] = 0.27;
    f.outPeak[0] = 0.78; f.outPeak[1] = 1.0; f.outRms[0] = 0.4; f.outRms[1] = 0.38;
    return f;
}

static void prime(WorkbenchView& wb, FakeBridge& bridge, double seconds, bool withMagnetics = true) {
    const int frames = static_cast<int>(seconds * 30.0) + 6;  // end between kicks so the last event can recover
    for (int i = 1; i <= frames; ++i) wb.updateFrame(richFrame(bridge, i / 30.0, withMagnetics), true);
}

static int distinctColours(const juce::Image& img) {
    std::set<juce::uint32> s;
    for (int y = 0; y < img.getHeight(); y += 3)
        for (int x = 0; x < img.getWidth(); x += 3) s.insert(img.getPixelAt(x, y).getARGB());
    return static_cast<int>(s.size());
}

static void snap(WorkbenchView& wb, const juce::File& dir, const juce::String& name, int w, int h, float scale) {
    auto img = cd::plugin::test::renderComponent(wb, w, h, scale);
    check(img.isValid() && img.getWidth() == juce::roundToInt(w * scale), name + ": image size");
    check(distinctColours(img) > 60, name + ": renders real content (" + juce::String(distinctColours(img)) + " colours)");
    check(cd::plugin::test::writePng(img, dir.getChildFile(name + ".png")), name + ": png written");
}

static bool inside(const juce::Component& c, const juce::Rectangle<int>& r) { return r.contains(c.getBounds()); }

static void testViewPreferences() {
    using cd::plugin::CircuitDriftProcessor;
    using cd::plugin::Edition;
    using cd::plugin::ProcessorEditorBridge;
    auto processor = std::make_unique<CircuitDriftProcessor>(Edition::Full, "CircuitDrift");
    ProcessorEditorBridge bridge(*processor);
    WorkbenchView workbench(bridge);
    workbench.setBounds(0, 0, 1000, 560);
    auto* canvas = dynamic_cast<PcbCanvas*>(workbench.findChildWithID("pcb"));
    check(canvas != nullptr, "live Workbench exposes its PCB canvas");
    if (!canvas) return;

    const auto viewButton = [](WorkbenchView& view, int mode) -> juce::TextButton* {
        const char* const labels[] = {"2D", "3D", "Sch"};
        for (auto* child : view.getChildren())
            if (auto* button = dynamic_cast<juce::TextButton*>(child);
                button && button->getButtonText() == labels[mode]) return button;
        return nullptr;
    };
    for (const int mode : {1, 2, 0}) {
        auto* button = viewButton(workbench, mode);
        check(button != nullptr, "view toolbar button exists");
        if (!button) continue;
        button->onClick();
        check(bridge.uiSettings().pcbView == mode && static_cast<int>(canvas->viewMode()) == mode,
              "view selection updates both persistent settings and live canvas");
        for (int candidate = 0; candidate < 3; ++candidate) {
            const auto* toggle = viewButton(workbench, candidate);
            check(toggle && toggle->getToggleState() == (candidate == mode),
                  "view toolbar selects only the active view");
        }

        juce::MemoryBlock state;
        processor->getStateInformation(state);
        auto restored = std::make_unique<CircuitDriftProcessor>(Edition::Full, "Restored");
        restored->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        ProcessorEditorBridge restoredBridge(*restored);
        WorkbenchView restoredView(restoredBridge);
        auto* restoredCanvas = dynamic_cast<PcbCanvas*>(restoredView.findChildWithID("pcb"));
        auto* restoredButton = viewButton(restoredView, mode);
        check(restoredCanvas && static_cast<int>(restoredCanvas->viewMode()) == mode &&
                  restoredButton && restoredButton->getToggleState(),
              "host session restores the selected PCB view and toolbar selection");
    }

    auto* threeD = viewButton(workbench, 1);
    if (!threeD) return;
    threeD->onClick();
    const auto originalCentre = canvas->centreOf(2);
    const juce::Point<float> start{-1000.0f, -1000.0f};
    const auto end = start + juce::Point<float>{36.0f, -18.0f};
    check(canvas->hitTest(start) < 0, "orbit gesture begins off all components");
    const juce::ModifierKeys left(juce::ModifierKeys::leftButtonModifier);
    canvas->pointerDown(start, left);
    canvas->pointerDrag(end, left);
    canvas->pointerUp(end);
    const auto orbitedCentre = canvas->centreOf(2);
    check(orbitedCentre.getDistanceFrom(originalCentre) > 0.1f, "orbit changes the projection");
    auto settings = bridge.uiSettings();
    settings.darkTheme = !settings.darkTheme;
    settings.reduceMotion = !settings.reduceMotion;
    bridge.setUiSettings(settings);
    workbench.settingsChanged(settings);
    check(canvas->viewMode() == PcbCanvas::ViewMode::Board3D &&
              canvas->centreOf(2).getDistanceFrom(orbitedCentre) < 0.0001f,
          "unrelated settings preserve the active view and orbit");
    threeD->onClick();
    check(canvas->centreOf(2).getDistanceFrom(orbitedCentre) < 0.0001f,
          "reselecting the active view does not reset the orbit");
}

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::File outDir(argc > 1 ? juce::String(argv[1]) : juce::File::getCurrentWorkingDirectory().getFullPathName());
    const auto dir = outDir.getChildFile("workbench");
    dir.createDirectory();

    testAnalysisMath();
    testGestureMath();
    testViewPreferences();

    FakeBridge bridge;
    WorkbenchView wb(bridge);
    wb.setBounds(0, 0, 1000, 560);
    auto* canvas = static_cast<PcbCanvas*>(wb.findChildWithID("pcb"));
    auto* inspector = static_cast<InspectorPanel*>(wb.findChildWithID("inspector"));
    auto* scope = static_cast<analysis::TransferScope*>(wb.findChildWithID("scope"));
    auto* timePlot = static_cast<analysis::TimePlot*>(wb.findChildWithID("timeplot"));
    auto* phase = static_cast<analysis::PhaseScatter*>(wb.findChildWithID("phase"));
    auto* meters = static_cast<analysis::LevelMeters*>(wb.findChildWithID("meters"));
    check(canvas && inspector && scope && timePlot && phase && meters, "workbench children present");
    if (!(canvas && inspector && scope && timePlot && phase && meters)) return 1;
    prime(wb, bridge, 3.0);

    // ---- displays carry honest labels/units ----
    {
        cd::plugin::test::renderComponent(wb, 1000, 560, 1.0f);
        check(scope->footerText().contains("slope") && scope->footerText().contains("FS"), "scope footer: slope and FS units: " + scope->footerText());
        check(timePlot->footerText().contains("DC rail") && timePlot->footerText().contains("not AC mains") && timePlot->footerText().contains("ms"),
              "rail footer separates DC rail from AC mains and reports recovery: " + timePlot->footerText());
        check(timePlot->recovery().status == analysis::RecoveryResult::Status::Recovered && std::abs(timePlot->recovery().recoveryMs - 50.0) <= 2.0,
              "UI recovery measurement within 2 ms of the injected 50 ms: " + juce::String(timePlot->recovery().recoveryMs, 1));
        check(phase->unavailableReason().isEmpty() && phase->footerText().contains("corr") && phase->footerText().contains("DC L-R"), "phase footer: " + phase->footerText());
        int visible = 0;
        for (const auto& b : phase->bins()) if (analysis::binVisible(b)) ++visible;
        check(visible > 10, "phase scatter has visible bins: " + juce::String(visible));
        check(scope->bhAvailable(), "B-H available when H/B telemetry is valid");
        check(near(meters->heldPeak(true, 1), 1.0, 1e-9) && meters->clipLit(true, 1), "meters: full-scale peak lights the clip lamp");
    }

    // ---- UX-01 gestures against the bridge (one transaction each) ----
    {
        auto* bias = bridge.parameters().getParameter(bridge.slotParamId("stage.bias"));
        auto* cap = bridge.parameters().getParameter(bridge.slotParamId("supply.railCapacity"));
        auto* age = bridge.parameters().getParameter(bridge.slotParamId("condition.ageYears"));
        auto* mat = bridge.parameters().getParameter(bridge.slotParamId("condition.resistorType"));
        check(bias && cap && age && mat, "gesture parameters exist");
        const std::size_t h0 = bridge.history().entries.size();
        const auto V1 = canvas->centreOf(2);

        bias->setValueNotifyingHost(0.0f);
        check(canvas->hitTest(V1) == 2, "tube hit-test at its centre");
        canvas->pointerDown(V1, juce::ModifierKeys());
        canvas->pointerDrag(V1 + juce::Point<float>(0.0f, -200.0f), juce::ModifierKeys());
        check(canvas->gestureActive(), "tube drag is a live gesture");
        canvas->pointerUp(V1);
        check(near(bias->getValue(), 1.0, 1e-4), "tube drag of 200 logical px up spans the full bias range: " + juce::String(bias->getValue(), 4));
        check(bridge.history().entries.size() == h0 + 1 && !canvas->gestureActive(), "tube drag = exactly one transaction");
        bias->setValueNotifyingHost(0.0f);
        canvas->pointerDown(V1, juce::ModifierKeys());
        canvas->pointerDrag(V1 + juce::Point<float>(0.0f, -200.0f), juce::ModifierKeys(juce::ModifierKeys::shiftModifier));
        canvas->pointerUp(V1);
        check(near(bias->getValue(), 0.1, 1e-4), "Shift = fine x0.1: " + juce::String(bias->getValue(), 4));
        // Escape rolls back and ends the host gesture.
        bias->setValueNotifyingHost(0.3f);
        canvas->pointerDown(V1, juce::ModifierKeys());
        canvas->pointerDrag(V1 + juce::Point<float>(0.0f, -60.0f), juce::ModifierKeys());
        canvas->cancelGesture();
        check(near(bias->getValue(), 0.3, 1e-4) && !canvas->gestureActive(), "Escape restores the pre-gesture value");
        canvas->pointerUp(V1);

        // Supply: sqrt2 doubles C, ratio clamps to 2 per gesture, parameter bounds clamp.
        canvas->select(12);
        cap->setValueNotifyingHost(cap->convertTo0to1(100.0f));
        const std::size_t h1 = bridge.history().entries.size();
        canvas->supplyBurst(std::sqrt(2.0));
        canvas->endBurst();
        check(near(cap->convertFrom0to1(cap->getValue()), 200.0, 0.2), "sqrt2 pinch doubles C: " + juce::String(cap->convertFrom0to1(cap->getValue()), 2));
        check(bridge.history().entries.size() == h1 + 1, "pinch burst = one transaction");
        canvas->supplyBurst(3.0); canvas->endBurst();
        check(near(cap->convertFrom0to1(cap->getValue()), 800.0, 1.0), "pinch ratio clamped to 2 -> C x4");
        canvas->supplyBurst(3.0); canvas->endBurst();
        check(near(cap->convertFrom0to1(cap->getValue()), 1000.0, 1.0), "C clamped to parameter maximum");

        // Age brush: path/400, Alt reverses; Material is a separate, explicit tool.
        canvas->setTool(PcbCanvas::Tool::AgeBrush);
        age->setValueNotifyingHost(0.0f);
        const auto Rk = canvas->centreOf(4);
        const std::size_t h2 = bridge.history().entries.size();
        canvas->pointerDown(Rk, juce::ModifierKeys());
        canvas->pointerDrag(Rk + juce::Point<float>(100.0f, 0.0f), juce::ModifierKeys());
        canvas->pointerDrag(Rk + juce::Point<float>(200.0f, 0.0f), juce::ModifierKeys());
        check(!canvas->brushSet().empty() && std::find(canvas->brushSet().begin(), canvas->brushSet().end(), 4) != canvas->brushSet().end(), "brush set frozen at pointer-down includes the hit resistor");
        canvas->pointerUp(Rk);
        check(near(age->getValue(), 0.5, 1e-3) && bridge.history().entries.size() == h2 + 1, "age brush 200 px = +0.5 of the fraction, one transaction");
        canvas->pointerDown(Rk, juce::ModifierKeys());
        canvas->pointerDrag(Rk + juce::Point<float>(100.0f, 0.0f), juce::ModifierKeys(juce::ModifierKeys::altModifier));
        canvas->pointerUp(Rk);
        check(near(age->getValue(), 0.25, 1e-3), "Alt reverses the brush");
        canvas->setTool(PcbCanvas::Tool::Auto);
        canvas->select(4);
        const std::size_t h3 = bridge.history().entries.size();
        canvas->setMaterial(2);
        check(near(mat->convertFrom0to1(mat->getValue()), 2.0, 1e-4) && bridge.history().entries.size() == h3 + 1, "material applied once, explicit enum choice");

        // Keyboard alternatives.
        canvas->select(2);
        bias->setValueNotifyingHost(0.5f);
        canvas->adjustSelected(+1, false);
        check(near(bias->getValue(), 0.51, 1e-4), "arrow = +1%");
        canvas->adjustSelected(-1, true);
        check(near(bias->getValue(), 0.509, 1e-4), "Shift+arrow = -0.1%");
        canvas->resetSelected();
        check(near(bias->getValue(), bias->getDefaultValue(), 1e-4), "Home resets to the factory base value");
        canvas->cycleSelection(+1);
        check(canvas->selected() == 3, "selection cycles with keys");

        // Inspector numeric entry: parses units/range, shows an error without changing state.
        canvas->select(12);
        inspector->refresh();
        check(inspector->applyNumeric("470u") && near(cap->convertFrom0to1(cap->getValue()), 470.0, 0.5), "numeric entry 470u -> 470 uF");
        const float before = cap->getValue();
        check(!inspector->applyNumeric("5000u") && inspector->lastError().isNotEmpty() && cap->getValue() == before, "out-of-range entry rejected without changing state");
        check(!inspector->applyNumeric("banana") && cap->getValue() == before, "garbage entry rejected without changing state");

        // Hit regions: >= 32 logical px, so +-15 px around every centre still hits a component.
        int missed = 0;
        for (const auto& part : pcb::parts()) {
            const auto c = canvas->centreOf(part.id);
            for (float dx : {-15.0f, 15.0f}) for (float dy : {-15.0f, 15.0f}) if (canvas->hitTest(c + juce::Point<float>(dx, dy)) < 0) ++missed;
            if (canvas->hitTest(c) != part.id) { ++failures; std::printf("FAIL: centre of %s resolves to %d\n", part.key, canvas->hitTest(c)); }
        }
        check(missed == 0, "hit regions are at least 32 px");
        canvas->select(-1);
    }

    // ---- B-H honesty (D10): unavailable without H/B telemetry ----
    {
        FakeBridge b2;
        WorkbenchView w2(b2);
        w2.setBounds(0, 0, 1000, 560);
        auto* s2 = static_cast<analysis::TransferScope*>(w2.findChildWithID("scope"));
        prime(w2, b2, 0.5, false);
        s2->setMode(analysis::TransferScope::Mode::MagneticBH);
        cd::plugin::test::renderComponent(w2, 1000, 560, 1.0f);
        check(!s2->bhAvailable() && s2->footerText().contains("unavailable"), "B-H labelled unavailable without magnetic telemetry: " + s2->footerText());
        snap(w2, dir, "wb_bh_unavailable", 1000, 560, 1.0f);
        // Mono processing: phase scatter L/R shows Not applicable (D16/UX-06); stereo without a ring says so.
        auto* p2 = static_cast<analysis::PhaseScatter*>(w2.findChildWithID("phase"));
        p2->setSource(analysis::PhaseScatter::Source::LeftRight);
        auto mono = richFrame(b2, 1.0);
        mono.inPeak[1] = mono.outPeak[1] = 0;
        w2.updateFrame(mono, true);
        check(p2->unavailableReason().startsWith("Not applicable"), "mono -> Not applicable: " + p2->unavailableReason());
        w2.updateFrame(richFrame(b2, 1.1), true);
        check(p2->unavailableReason().contains("L/R sample ring"), "stereo L/R without ring says so: " + p2->unavailableReason());
        snap(w2, dir, "wb_phase_mono_na", 1000, 560, 1.0f);
    }

    // ---- stale badge ----
    {
        canvas->setStaleOverrideForTest(true);
        wb.updateFrame(richFrame(bridge, 3.2), false);
        check(canvas->stale(), "invalid frame past the threshold shows Stale");
        snap(wb, dir, "wb_stale", 1000, 560, 1.0f);
        canvas->clearStaleOverride();
        wb.updateFrame(richFrame(bridge, 3.3), true);
        check(!canvas->stale(), "new frame clears Stale");
        canvas->setStaleOverrideForTest(false);  // snapshot rendering is slower than live frame cadence: pin fresh
    }

    // ---- snapshots: overlays x themes ----
    const char* overlayNames[4] = {"off", "thermal", "voltage", "signal"};
    for (int dark = 1; dark >= 0; --dark) {
        for (int ov = 0; ov < 4; ++ov) {
            cd::plugin::UiSettings s;
            s.darkTheme = dark == 1; s.overlay = ov;
            bridge.setUiSettings(s);
            wb.settingsChanged(s);
            canvas->select(2);
            wb.updateFrame(richFrame(bridge, 3.4), true);
            snap(wb, dir, juce::String("wb_") + (dark ? "dark_" : "light_") + overlayNames[ov] + "_V1", 1000, 560, 1.0f);
        }
    }
    {
        // Inspector text carries identity, provenance and units.
        cd::plugin::UiSettings s;
        bridge.setUiSettings(s); wb.settingsChanged(s);
        canvas->select(12);
        wb.updateFrame(richFrame(bridge, 3.5), true);
        cd::plugin::test::renderComponent(wb, 1000, 560, 1.0f);
        const auto txt = inspector->drawnText();
        check(txt.contains("Reservoir") && txt.contains("synthetic") && txt.contains("reduced-supply") && txt.contains("F") && txt.contains("V_res"), "inspector shows identity, model, provenance, units");
        check(txt.contains("Nominal") && txt.contains("Base") && txt.contains("Effective"), "inspector shows nominal/base/effective");
        canvas->select(2);
        cd::plugin::test::renderComponent(wb, 1000, 560, 1.0f);
        const auto tv = inspector->drawnText();
        check(tv.contains(juce::String(juce::CharPointer_UTF8("\xC2\xB0")) + "C") && tv.contains("Dissipation") && tv.contains("Stage current"), "inspector shows live temperature, power, current");
    }

    // ---- selection / X-Ray / projections / schematic ----
    {
        cd::plugin::UiSettings s;
        bridge.setUiSettings(s); wb.settingsChanged(s);
        canvas->select(12);
        snap(wb, dir, "wb_dark_thermal_C_res_inspector", 1000, 560, 1.0f);
        canvas->select(4);
        canvas->setTool(PcbCanvas::Tool::Material);
        snap(wb, dir, "wb_dark_thermal_R_k_material_tool", 1000, 560, 1.0f);
        canvas->setTool(PcbCanvas::Tool::Auto);

        s.xray = true; bridge.setUiSettings(s); wb.settingsChanged(s);
        canvas->select(2);
        snap(wb, dir, "wb_dark_thermal_xray", 1000, 560, 1.0f);
        s.xray = false; s.overlay = 2; bridge.setUiSettings(s); wb.settingsChanged(s);

        s.pcbView = 1; bridge.setUiSettings(s); wb.settingsChanged(s);
        for (int ov = 1; ov <= 3; ++ov) {
            s.overlay = ov; bridge.setUiSettings(s); wb.settingsChanged(s);
            snap(wb, dir, juce::String("wb_dark_3d_") + overlayNames[ov], 1000, 560, 1.0f);
        }
        s.overlay = 1; s.darkTheme = false; bridge.setUiSettings(s); wb.settingsChanged(s);
        snap(wb, dir, "wb_light_3d_thermal", 1000, 560, 1.0f);
        s.darkTheme = true; s.xray = true; bridge.setUiSettings(s); wb.settingsChanged(s);
        snap(wb, dir, "wb_dark_3d_xray", 1000, 560, 1.0f);
        // 3D hit-testing stays consistent with the projection.
        int bad = 0;
        for (const auto& part : pcb::parts()) if (canvas->hitTest(canvas->centreOf(part.id)) < 0) ++bad;
        check(bad == 0, "3D projected centres hit their components");
        s.xray = false; bridge.setUiSettings(s); wb.settingsChanged(s);

        s.pcbView = 2; bridge.setUiSettings(s); wb.settingsChanged(s);
        for (int dark = 1; dark >= 0; --dark) {
            s.darkTheme = dark == 1; s.overlay = 1; bridge.setUiSettings(s); wb.settingsChanged(s);
            snap(wb, dir, juce::String("wb_") + (dark ? "dark" : "light") + "_schematic", 1000, 560, 1.0f);
        }
        s.pcbView = 0; bridge.setUiSettings(s); wb.settingsChanged(s);
    }

    // ---- scales 100/150/200 and layout invariants at the minimum viewport ----
    {
        cd::plugin::UiSettings s;
        bridge.setUiSettings(s); wb.settingsChanged(s);
        canvas->select(2);
        for (float scale : {1.0f, 1.5f, 2.0f}) snap(wb, dir, "wb_dark_thermal_scale" + juce::String(juce::roundToInt(scale * 100)), 1000, 560, scale);
        s.darkTheme = false; bridge.setUiSettings(s); wb.settingsChanged(s);
        for (float scale : {1.0f, 1.5f, 2.0f}) snap(wb, dir, "wb_light_thermal_scale" + juce::String(juce::roundToInt(scale * 100)), 1000, 560, scale);
        s.darkTheme = true; bridge.setUiSettings(s); wb.settingsChanged(s);
        // Narrow layout (<900): inspector becomes a drawer; everything stays inside the view.
        for (auto size : {juce::Point<int>(750, 540), juce::Point<int>(800, 560), juce::Point<int>(1000, 560)}) {
            wb.setBounds(0, 0, size.x, size.y);
            canvas->select(12);
            snap(wb, dir, "wb_dark_" + juce::String(size.x) + "x" + juce::String(size.y), size.x, size.y, 1.0f);
            const auto r = wb.getLocalBounds();
            bool allIn = true;
            for (auto* c : wb.getChildren()) if (c->isVisible() && !inside(*c, r)) { allIn = false; std::printf("  outside: %s\n", c->getComponentID().toRawUTF8()); }
            check(allIn, "all visible controls inside the view at " + juce::String(size.x) + "x" + juce::String(size.y));
            if (size.x < 900) check(inspector->isVisible(), "inspector drawer opens on selection when narrow");
            for (auto* c : wb.getChildren())
                if (auto* tb = dynamic_cast<juce::TextButton*>(c); tb && tb->isVisible()) check(tb->getWidth() >= 28 && tb->getHeight() >= 24, "toolbar button large enough: " + tb->getButtonText());
        }
    }

    std::printf(failures ? "Workbench snapshot FAILED (%d)\n" : "Workbench snapshot passed\n", failures);
    return failures ? 1 : 0;
}
