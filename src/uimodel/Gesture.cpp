// CircuitDrift uimodel: UX-01 gesture controller + numeric entry parser. Owner: cd_uimodel.
// Message thread only (allocates).
#include "uimodel/Gesture.h"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace cd::uimodel {

namespace {
double clampd(double v, double lo, double hi) noexcept { return std::fmin(hi, std::fmax(lo, v)); }
bool finite2(Point p) noexcept { return std::isfinite(p.x) && std::isfinite(p.y); }
double dist(Point a, Point b) noexcept { return std::hypot(a.x - b.x, a.y - b.y); }
std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}
NumericResult fail(std::string why) { NumericResult r; r.error = std::move(why); return r; }
}  // namespace

std::string noiseLinkId(const std::string& id) { return "excessNoise." + id; }
std::string ageId(const std::string& id) { return "age." + id; }

NumericResult parseNumeric(const std::string& textIn, double min, double max, const std::string& unit, bool discrete) {
    const std::string text = trim(textIn);
    if (text.empty()) return fail("empty entry");
    if (text.size() > 64) return fail("entry too long");
    double v = 0;
    const std::string lc = lower(text);
    if (unit == "bool" && (lc == "on" || lc == "true" || lc == "yes" || lc == "off" || lc == "false" || lc == "no")) {
        v = (lc == "on" || lc == "true" || lc == "yes") ? 1.0 : 0.0;
    } else {
        const char* b = text.data();
        const char* e = b + text.size();
        if (b != e && *b == '+') ++b;
        const auto r = std::from_chars(b, e, v);
        if (r.ec != std::errc() || r.ptr == b) return fail("not a number");
        if (!std::isfinite(v)) return fail("not a finite number");
        const std::string suf = trim(std::string(r.ptr, e));
        const std::string s = lower(suf);
        if (unit == "Hz") {
            if (s == "" || s == "hz") {}
            else if (s == "k" || s == "khz") v *= 1000.0;
            else return fail("unit must be Hz or kHz");
        } else if (unit == "dB") {
            if (!(s == "" || s == "db")) return fail("unit must be dB");
        } else if (unit == "normalized") {
            if (s == "%") v /= 100.0;
            else if (s != "") return fail("unit must be a plain number or %");
        } else if (!s.empty()) {
            return fail("no unit expected for " + unit);
        }
    }
    if (!std::isfinite(v)) return fail("not a finite number");
    if (discrete && std::fabs(v - std::round(v)) > 1e-9) return fail("must be a whole number");
    if (v < min || v > max) {
        char buf[128];
        std::snprintf(buf, sizeof buf, "out of range [%g, %g] %s", min, max, unit.c_str());
        return fail(buf);
    }
    NumericResult ok;
    ok.ok = true;
    ok.value = discrete ? std::round(v) : v;
    return ok;
}

NumericResult parseNumeric(const std::string& text, const core::ParamSpec& s) {
    return parseNumeric(text, s.min, s.max, s.unit, s.kind != core::ParamKind::Float);
}

GestureController::GestureController(ParamDocument& doc, state::History& history, EditHost& host, ClockUs clock, GestureConfig cfg)
    : doc_(doc), hist_(history), host_(host), clock_(std::move(clock)), cfg_(std::move(cfg)) {
    if (!std::isfinite(cfg_.uiScale)) cfg_.uiScale = 1.0;
    cfg_.uiScale = clampd(cfg_.uiScale, 0.75, 2.0);
    if (cfg_.minPublishIntervalUs < 8334) cfg_.minPublishIntervalUs = 8334;  // never above 120 Hz
}

bool GestureController::open(GestureKind k, std::vector<std::string> ids) {
    if (active()) return false;
    for (const auto& id : ids) if (!doc_.find(id)) return false;
    kind_ = k;
    ids_ = std::move(ids);
    start_.clear();
    for (const auto& id : ids_) start_.push_back(doc_.get(id));
    for (const auto& id : ids_) host_.beginEdit(id);
    dirty_ = false;
    havePublished_ = false;
    ++gestures_;
    return true;
}

void GestureController::publish(bool force) {
    if (!dirty_) return;
    const std::int64_t now = clock_ ? clock_() : 0;
    if (!force && havePublished_ && now - lastPublishUs_ < cfg_.minPublishIntervalUs) return;
    for (const auto& id : ids_) host_.performEdit(id, doc_.get(id));
    dirty_ = false;
    havePublished_ = true;
    lastPublishUs_ = now;
    ++publishes_;
}

bool GestureController::beginBias(Point p) {
    if (!finite2(p) || !open(GestureKind::Bias, {cfg_.biasId})) return false;
    last_ = p;
    raw_ = doc_.normalized(cfg_.biasId);
    return true;
}

void GestureController::moveBias(Point p, bool shift) {
    if (kind_ != GestureKind::Bias || !finite2(p)) return;
    const double dyLogical = (p.y - last_.y) / cfg_.uiScale;
    last_ = p;
    raw_ += -dyLogical / 200.0 * (shift ? 0.1 : 1.0);
    const Entry* e = doc_.find(cfg_.biasId);
    doc_.set(cfg_.biasId, e->min + clampd(raw_, 0.0, 1.0) * (e->max - e->min));
    dirty_ = true;
    publish(false);
}

bool GestureController::beginPinch(Point a, Point b) {
    if (!finite2(a) || !finite2(b) || !(dist(a, b) > 0)) return false;
    if (!open(GestureKind::Pinch, {cfg_.supplyCapId, cfg_.sagId})) return false;
    d0_ = dist(a, b);
    return true;
}

void GestureController::movePinch(Point a, Point b) {
    if (kind_ != GestureKind::Pinch || !finite2(a) || !finite2(b)) return;
    const double r = clampd(dist(a, b) / d0_, 0.5, 2.0);
    doc_.set(cfg_.supplyCapId, start_[0] * r * r);          // set() clamps to manifest bounds
    doc_.set(cfg_.sagId, start_[1] + (-std::log2(r) / 4.0));  // set() clamps to 0..1
    dirty_ = true;
    publish(false);
}

bool GestureController::beginBrush(Point p, const std::vector<std::string>& ids, bool alt) {
    if (!finite2(p) || active()) return false;
    std::vector<std::string> comps, all;
    for (const auto& c : ids) {
        if (std::find(comps.begin(), comps.end(), c) != comps.end()) continue;
        if (!doc_.find(ageId(c)) || !doc_.find(noiseLinkId(c))) continue;
        comps.push_back(c);
        all.push_back(ageId(c));
        all.push_back(noiseLinkId(c));
    }
    if (comps.empty() || !open(GestureKind::AgeBrush, all)) return false;
    brushIds_ = std::move(comps);  // frozen at pointer-down
    last_ = p;
    path_ = 0;
    alt_ = alt;
    return true;
}

void GestureController::moveBrush(Point p) {
    if (kind_ != GestureKind::AgeBrush || !finite2(p)) return;
    path_ += dist(p, last_) / cfg_.uiScale;
    last_ = p;
    const double delta = (alt_ ? -1.0 : 1.0) * path_ / 400.0;
    for (std::size_t i = 0; i < ids_.size(); ++i) {
        const Entry* e = doc_.find(ids_[i]);
        doc_.set(ids_[i], clampd(start_[i] + delta * (e->max - e->min), e->min, e->max));
    }
    dirty_ = true;
    publish(false);
}

bool GestureController::finish(bool commit) {
    if (!active()) return false;
    if (commit) {
        publish(true);
    } else {
        for (std::size_t i = 0; i < ids_.size(); ++i) {
            doc_.set(ids_[i], start_[i]);
            if (havePublished_) host_.performEdit(ids_[i], start_[i]);
        }
        dirty_ = false;
    }
    for (const auto& id : ids_) host_.endEdit(id);
    if (commit) {
        const char* label = kind_ == GestureKind::Bias ? "Adjust tube bias"
                          : kind_ == GestureKind::Pinch ? "Adjust supply capacitance"
                                                        : "Age brush";
        hist_.commit(label, state::TxOrigin::Gesture, ids_, doc_.blob());
    }
    kind_ = GestureKind::None;
    brushIds_.clear();
    return true;
}

bool GestureController::end() { return finish(true); }
bool GestureController::pointerLost() { return finish(true); }
bool GestureController::cancel() { return finish(false); }

bool GestureController::oneShot(const std::string& label, const std::vector<std::pair<std::string, double>>& changes) {
    if (active() || changes.empty()) return false;
    std::vector<std::string> ids;
    for (const auto& c : changes) {
        if (!doc_.find(c.first) || !std::isfinite(c.second)) return false;
        ids.push_back(c.first);
    }
    std::vector<double> prev;
    for (const auto& id : ids) prev.push_back(doc_.get(id));
    for (const auto& c : changes) doc_.set(c.first, c.second);
    for (const auto& id : ids) { host_.beginEdit(id); host_.performEdit(id, doc_.get(id)); host_.endEdit(id); }
    ++publishes_;
    ++gestures_;
    const auto r = hist_.commit(label, state::TxOrigin::Gesture, ids, doc_.blob());
    return r == state::CommitResult::Recorded || r == state::CommitResult::NoChange;
}

bool GestureController::selectMaterial(const std::string& id, int idx) {
    const Entry* e = doc_.find(id);
    if (!e || idx < e->min || idx > e->max) return false;
    return oneShot("Select material", {{id, double(idx)}});
}

bool GestureController::enterNumeric(const std::string& id, const std::string& text, std::string* error) {
    const Entry* e = doc_.find(id);
    if (!e) { if (error) *error = "unknown control"; return false; }
    const core::ParamSpec* spec = core::findById(id.c_str());
    const NumericResult r = spec ? parseNumeric(text, *spec) : parseNumeric(text, e->min, e->max, "", e->discrete);
    if (!r.ok) { if (error) *error = r.error; return false; }  // state untouched
    return oneShot("Enter value", {{id, r.value}});
}

bool GestureController::step(const std::string& id, int direction, bool shift) {
    const Entry* e = doc_.find(id);
    if (!e || direction == 0) return false;
    const double sgn = direction > 0 ? 1.0 : -1.0;
    double target;
    if (e->discrete) target = e->value + sgn;  // enums/bools step by one
    else target = e->value + sgn * (shift ? 0.001 : 0.01) * (e->max - e->min);
    return oneShot("Nudge value", {{id, target}});
}

bool GestureController::resetToFactory(const std::string& id) {
    const Entry* e = doc_.find(id);
    return e && oneShot("Reset to factory", {{id, e->factory}});
}

}  // namespace cd::uimodel
