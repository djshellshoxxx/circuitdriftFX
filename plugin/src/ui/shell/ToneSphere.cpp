#include "ui/shell/ToneSphere.h"

#include "ui/shell/HelpOverlay.h"

namespace cd::plugin::ui::shell {

SphereTargets computeSphereTargets(const SphereInputs& in) noexcept {
    SphereTargets t;
    const bool haveRail = in.valid && in.railNominalVolts > 0.0;
    t.railFraction = haveRail ? std::clamp(in.railVolts / in.railNominalVolts, 0.0, 1.0) : 1.0;
    t.rmsDb = in.outRmsLinear > 1e-6 ? 20.0 * std::log10(in.outRmsLinear) : -120.0;
    t.silent = !in.valid || t.rmsDb < -80.0;
    t.controlPreview = in.measuredResidual < 0.0;
    t.colourT = std::clamp(t.controlPreview ? in.warmth : in.measuredResidual, 0.0, 1.0);

    if (in.reduceMotion) {            // numeric indicators stay; deformation and pulsing stop
        t.radius = 0.90;
        t.scaleX = t.scaleY = 1.0;
        t.edgeAmp = 0.0;
    } else if (t.silent) {            // calm ring
        t.radius = 0.85;
        t.scaleX = t.scaleY = 1.0;
        t.edgeAmp = 0.0;
    } else {
        const double squash = 0.25 * (1.0 - t.railFraction);
        t.radius = 0.85 + 0.10 * std::clamp(t.rmsDb + 60.0, 0.0, 60.0) / 60.0;
        t.scaleY = 1.0 - squash;
        t.scaleX = 1.0 + 0.5 * squash;
        t.edgeAmp = std::clamp(0.03 * (0.5 * in.age + 0.5 * in.drift), 0.0, 0.03);
    }
    return t;
}

juce::Colour sphereColour(double t, const Theme& theme) {
    const auto cyan = theme.dark ? juce::Colour(0xff35d0e6) : juce::Colour(0xff0a8ea6);
    const auto amber = theme.dark ? juce::Colour(0xffe8a33d) : juce::Colour(0xffd98a12);
    const auto crimson = theme.dark ? juce::Colour(0xffc4203a) : juce::Colour(0xffb01030);
    t = std::clamp(t, 0.0, 1.0);
    return t < 0.5 ? cyan.interpolatedWith(amber, static_cast<float>(t * 2.0))
                   : amber.interpolatedWith(crimson, static_cast<float>((t - 0.5) * 2.0));
}

ToneSphere::ToneSphere(ShellContext& ctx) : ctx_(ctx) {
    juce::Random rng(0x70E57A7E);  // render-only RNG: never shared with or advancing the audio RNG
    for (int i = 0; i < 3; ++i) {
        phi_[i] = rng.nextDouble() * juce::MathConstants<double>::twoPi;
        omega_[i] = 0.35 + 0.5 * rng.nextDouble();
    }
    setTitle("Tone State sphere");
    setTip(*this, "Tone State",
                "An expressive summary of the sound, not a calibrated meter. Size follows level, squash follows "
                "supply sag, colour follows Warmth (control preview until a measured reference exists), and edge "
                "ripple follows Age and Drift.");
}

void ToneSphere::update(const SphereInputs& in, double dt) {
    in_ = in;
    target_ = computeSphereTargets(in);
    const double a = primed_ ? 1.0 - std::exp(-std::max(0.0, dt) / kSphereSmoothingSeconds) : 1.0;
    auto mix = [a](double& s, double t) { s += (t - s) * a; };
    mix(smooth_.railFraction, target_.railFraction);
    mix(smooth_.rmsDb, target_.rmsDb);
    mix(smooth_.radius, target_.radius);
    mix(smooth_.scaleX, target_.scaleX);
    mix(smooth_.scaleY, target_.scaleY);
    mix(smooth_.colourT, target_.colourT);
    mix(smooth_.edgeAmp, target_.edgeAmp);
    smooth_.controlPreview = target_.controlPreview;
    smooth_.silent = target_.silent;
    primed_ = true;
    if (!in.reduceMotion && !target_.silent) phase_ += dt;  // ripple animates only while enabled and active

    const auto text = "Rail " + juce::String(juce::roundToInt(smooth_.railFraction * 100.0)) + " percent of nominal, level " +
                      (target_.silent ? juce::String("silent") : juce::String(juce::roundToInt(smooth_.rmsDb)) + " dBFS") +
                      ", warmth " + juce::String(juce::roundToInt(in.warmth * 100.0)) + " percent" +
                      (smooth_.controlPreview ? " control preview" : "");
    if (text != describedText_) {
        describedText_ = text;
        setDescription(text);
    }
    repaint();
}

double ToneSphere::edgeFactor(double theta) const noexcept {
    const double w[3] = {0.5, 0.3, 0.2};
    const int k[3] = {3, 5, 8};
    double s = 0.0;
    for (int i = 0; i < 3; ++i) s += w[i] * std::sin(k[i] * theta + phi_[i] + omega_[i] * phase_);
    return 1.0 + smooth_.edgeAmp * s;  // |s| <= 1, so the factor stays within 1 +/- edgeAmp (<= 3 %)
}

void ToneSphere::paint(juce::Graphics& g) {
    const auto& t = ctx_.theme();
    auto area = getLocalBounds().toFloat();
    const bool wide = area.getWidth() > area.getHeight() * 1.45f;

    // ---- Readout columns (always shown: numbers remain when motion is reduced) ----
    auto readout = [&](juce::Rectangle<float> r, const juce::String& label, const juce::String& value, double fraction,
                       juce::Colour bar, const juce::String& note = {}) {
        drawPlate(g, r, t, false, 4.0f);
        auto in = r.reduced(8.0f, 5.0f);
        drawEngraved(g, label.toUpperCase(), in.removeFromTop(12.0f), uiFont(10.0f, true).withExtraKerningFactor(0.06f), t,
                     juce::Justification::centredLeft, t.textDim);
        g.setFont(monoFont(15.0f, true));
        g.setColour(t.text);
        g.drawText(value, in.removeFromTop(19.0f), juce::Justification::centredLeft, true);
        if (note.isNotEmpty()) {
            g.setFont(uiFont(10.0f));
            g.setColour(t.textDim);
            g.drawText(note, in.removeFromTop(12.0f), juce::Justification::centredLeft, true);
        }
        if (fraction >= 0.0) {
            auto bar_r = juce::Rectangle<float>(r.getX() + 8.0f, r.getBottom() - 10.0f, r.getWidth() - 16.0f, 4.0f);
            g.setColour(t.inset.darker(t.dark ? 0.4f : 0.1f));
            g.fillRoundedRectangle(bar_r, 2.0f);
            g.setColour(bar);
            g.fillRoundedRectangle(bar_r.withWidth(bar_r.getWidth() * static_cast<float>(std::clamp(fraction, 0.0, 1.0))), 2.0f);
        }
    };

    juce::Rectangle<float> sphereArea = area;
    const float rh = 62.0f;
    const auto railPct = juce::roundToInt(smooth_.railFraction * 100.0);
    const auto levelText = target_.silent ? juce::String("silent") : juce::String(smooth_.rmsDb, 1) + " dBFS";
    const juce::String warmText = juce::String(juce::roundToInt(in_.warmth * 100.0)) + " %";
    const juce::String edgeText = juce::String(smooth_.edgeAmp * 100.0, 1) + " %";
    if (wide) {
        const float cw = std::min(170.0f, area.getWidth() * 0.2f);
        auto left = area.removeFromLeft(cw).reduced(4.0f, 0.0f);
        auto right = area.removeFromRight(cw).reduced(4.0f, 0.0f);
        sphereArea = area;
        const float gapY = 8.0f;
        const float topY = left.getCentreY() - (rh * 3.0f + gapY * 2.0f) * 0.5f;
        readout({left.getX(), topY, left.getWidth(), rh}, "Rail", juce::String(railPct) + " %", smooth_.railFraction, t.accent, "of nominal");
        readout({left.getX(), topY + rh + gapY, left.getWidth(), rh}, "Sag macro", juce::String(juce::roundToInt(in_.sag * 100.0)) + " %", in_.sag, t.accent);
        readout({left.getX(), topY + (rh + gapY) * 2.0f, left.getWidth(), rh}, "Level", levelText,
                std::clamp((smooth_.rmsDb + 60.0) / 60.0, 0.0, 1.0), t.signal);
        readout({right.getX(), topY, right.getWidth(), rh}, "Warmth", warmText, in_.warmth, sphereColour(smooth_.colourT, t),
                smooth_.controlPreview ? "control preview" : "measured");
        readout({right.getX(), topY + rh + gapY, right.getWidth(), rh}, "Age + drift", juce::String(juce::roundToInt(in_.age * 100.0)) + " / " + juce::String(juce::roundToInt(in_.drift * 100.0)) + " %",
                0.5 * (in_.age + in_.drift), t.voltagePos, "edge ripple " + edgeText);
        readout({right.getX(), topY + (rh + gapY) * 2.0f, right.getWidth(), rh}, "Squash", juce::String(juce::roundToInt((1.0 - smooth_.scaleY) * 100.0)) + " %",
                (1.0 - smooth_.scaleY) / 0.25, t.warning, in_.reduceMotion ? "motion reduced" : "from rail sag");
    } else {
        auto bottom = area.removeFromBottom(rh * 0.62f);
        sphereArea = area;
        const float cw = bottom.getWidth() / 3.0f;
        auto mini = [&](int i, const juce::String& label, const juce::String& value) {
            auto r = juce::Rectangle<float>(bottom.getX() + cw * static_cast<float>(i), bottom.getY(), cw, bottom.getHeight()).reduced(3.0f, 0.0f);
            drawPlate(g, r, t, false, 4.0f);
            auto in = r.reduced(6.0f, 2.0f);
            drawEngraved(g, label.toUpperCase(), in.removeFromTop(11.0f), uiFont(9.5f, true), t, juce::Justification::centredLeft, t.textDim);
            g.setFont(monoFont(12.5f, true));
            g.setColour(t.text);
            g.drawText(value, in, juce::Justification::centredLeft, true);
        };
        mini(0, "Rail", juce::String(railPct) + " %");
        mini(1, "Sag", juce::String(juce::roundToInt(in_.sag * 100.0)) + " %");
        mini(2, "Warmth", warmText + (smooth_.controlPreview ? " preview" : ""));
    }

    // ---- Porthole ----
    const float side = std::min(sphereArea.getWidth(), sphereArea.getHeight());
    const auto c = sphereArea.getCentre();
    const float Rp = side * 0.5f - 2.0f;
    if (Rp < 20.0f) return;
    juce::Rectangle<float> outer(c.x - Rp, c.y - Rp, Rp * 2, Rp * 2);
    g.setColour(juce::Colours::black.withAlpha(t.dark ? 0.6f : 0.25f));
    g.fillEllipse(outer.translated(0.0f, 3.0f));
    g.setGradientFill(juce::ColourGradient(t.dark ? t.panelRaised.brighter(0.8f) : juce::Colours::white, outer.getX(), outer.getY(),
                                           t.dark ? t.inset : t.border, outer.getRight(), outer.getBottom(), false));
    g.fillEllipse(outer);
    g.setColour(t.border);
    g.drawEllipse(outer, 1.2f);
    const float lensR = Rp * 0.90f;
    juce::Rectangle<float> lens(c.x - lensR, c.y - lensR, lensR * 2, lensR * 2);
    const auto lensTop = t.dark ? t.inset.darker(0.5f) : t.inset.darker(0.35f);
    g.setGradientFill(juce::ColourGradient(t.dark ? t.inset.brighter(0.15f) : t.inset, c.x, c.y - lensR * 0.2f, lensTop, c.x + lensR, c.y + lensR, true));
    g.fillEllipse(lens);

    // Bezel ticks.
    for (int i = 0; i < 72; ++i) {
        const float a = juce::MathConstants<float>::twoPi * static_cast<float>(i) / 72.0f;
        const bool major = i % 6 == 0;
        const float r0 = Rp * 0.935f, r1 = Rp * (major ? 0.985f : 0.96f);
        g.setColour(major ? t.textDim : t.border.withAlpha(0.7f));
        g.drawLine(c.x + r0 * std::sin(a), c.y - r0 * std::cos(a), c.x + r1 * std::sin(a), c.y - r1 * std::cos(a), major ? 1.3f : 0.8f);
    }

    juce::Graphics::ScopedSaveState clip(g);
    juce::Path lensClip;
    lensClip.addEllipse(lens);
    g.reduceClipRegion(lensClip);

    // Reticle: nominal-size ring + cross + range rings (the 0.85 ring is the calm/idle size).
    g.setColour(t.grid.withAlpha(t.dark ? 0.55f : 0.9f));
    for (float f : {0.33f, 0.66f}) g.drawEllipse(c.x - lensR * f, c.y - lensR * f, lensR * 2 * f, lensR * 2 * f, 0.8f);
    g.drawLine(c.x - lensR, c.y, c.x + lensR, c.y, 0.8f);
    g.drawLine(c.x, c.y - lensR, c.x, c.y + lensR, 0.8f);

    const float baseR = lensR * 0.80f;
    const auto col = sphereColour(smooth_.colourT, t);
    const float r = baseR * static_cast<float>(smooth_.radius);
    // Calm reference ring.
    const float calm = baseR * 0.85f;
    g.setColour(col.withAlpha(0.45f));
    {
        juce::Path ring;
        ring.addEllipse(c.x - calm, c.y - calm, calm * 2, calm * 2);
        juce::Path dashed;
        const float dashes[] = {4.0f, 4.0f};
        juce::PathStrokeType(1.0f).createDashedStroke(dashed, ring, dashes, 2);
        g.fillPath(dashed);
    }

    // Glow.
    g.setGradientFill(juce::ColourGradient(col.withAlpha(t.dark ? 0.55f : 0.40f), c.x, c.y, col.withAlpha(0.0f), c.x + r * 1.7f, c.y, true));
    g.fillEllipse(c.x - r * 1.7f, c.y - r * 1.7f, r * 3.4f, r * 3.4f);

    // Body outline with ripple.
    juce::Path body;
    const int n = 120;
    for (int i = 0; i < n; ++i) {
        const double th = juce::MathConstants<double>::twoPi * i / n;
        const float rr = r * static_cast<float>(edgeFactor(th));
        const float x = c.x + rr * static_cast<float>(smooth_.scaleX) * static_cast<float>(std::sin(th));
        const float y = c.y - rr * static_cast<float>(smooth_.scaleY) * static_cast<float>(std::cos(th));
        i == 0 ? body.startNewSubPath(x, y) : body.lineTo(x, y);
    }
    body.closeSubPath();
    g.setGradientFill(juce::ColourGradient(col.brighter(0.55f), c.x - r * 0.35f, c.y - r * 0.45f, col.darker(0.75f), c.x + r * 0.7f,
                                           c.y + r * 0.8f, true));
    g.fillPath(body);
    {   // Limb darkening so the disc reads as a sphere.
        juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(body);
        g.setGradientFill(juce::ColourGradient(juce::Colours::black.withAlpha(0.0f), c.x - r * 0.2f, c.y - r * 0.25f,
                                               juce::Colours::black.withAlpha(t.dark ? 0.55f : 0.40f), c.x + r * 1.0f, c.y + r * 1.0f, true));
        g.fillRect(c.x - r * 1.5f, c.y - r * 1.5f, r * 3.0f, r * 3.0f);
    }
    g.setColour(col.brighter(0.4f));
    g.strokePath(body, juce::PathStrokeType(1.6f));
    // Specular highlight.
    juce::Rectangle<float> hl(c.x - r * 0.55f, c.y - r * 0.68f, r * 0.55f, r * 0.32f);
    g.setGradientFill(juce::ColourGradient(juce::Colours::white.withAlpha(0.35f), hl.getCentreX(), hl.getCentreY(),
                                           juce::Colours::white.withAlpha(0.0f), hl.getX(), hl.getY(), true));
    g.fillEllipse(hl);
    // Phosphor scanlines.
    g.setColour(juce::Colours::black.withAlpha(t.dark ? 0.12f : 0.05f));
    for (float y = c.y - lensR; y < c.y + lensR; y += 3.0f) g.fillRect(c.x - lensR, y, lensR * 2, 1.0f);
}

}  // namespace cd::plugin::ui::shell
