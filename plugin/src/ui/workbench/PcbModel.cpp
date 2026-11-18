#include "ui/workbench/PcbModel.h"

#include <algorithm>

namespace cd::plugin::ui::pcb {

const std::array<Part, kPartCount>& parts() {
    // Order == engine::ComponentId. Thermal references are illustrative per-component values
    // (synthetic fixture, G01 open): {cool below, normal up to, stress from} in deg C.
    static const std::array<Part, kPartCount> p = {{
        {0, "C_in", Kind::FilmCap, 12, 30, 12, 6, 5, {30, 55, 85}, "film cap ref 30/55/85 C"},
        {1, "R_g", Kind::Resistor, 24, 42, 4.5, 12, 4, {35, 70, 105}, "resistor ref 35/70/105 C"},
        {2, "V1", Kind::Tube, 44, 26, 18, 18, 22, {60, 130, 170}, "tube envelope ref 60/130/170 C"},
        {3, "R_load", Kind::Resistor, 70, 10, 4.5, 13, 4, {35, 75, 110}, "resistor ref 35/75/110 C"},
        {4, "R_k", Kind::Resistor, 47, 49, 4.5, 8, 4, {35, 70, 105}, "resistor ref 35/70/105 C"},
        {5, "C_k", Kind::Electrolytic, 56, 49, 8, 8, 12, {30, 55, 85}, "electrolytic ref 30/55/85 C"},
        {6, "C_out", Kind::FilmCap, 74, 30, 12, 6, 5, {30, 55, 85}, "film cap ref 30/55/85 C"},
        {7, "T_out", Kind::Transformer, 96, 22, 20, 20, 16, {35, 75, 105}, "transformer winding ref 35/75/105 C"},
        {8, "C_dec", Kind::Electrolytic, 100, 5, 10, 10, 13, {30, 55, 85}, "electrolytic ref 30/55/85 C"},
        {9, "R_dec", Kind::Resistor, 84, 8, 12, 4.5, 4, {35, 75, 110}, "resistor ref 35/75/110 C"},
        {10, "R_g2", Kind::Resistor, 28, 10, 12, 4.5, 4, {35, 70, 105}, "resistor ref 35/70/105 C"},
        {11, "C_g2", Kind::FilmCap, 44, 16, 10, 5, 4, {30, 55, 85}, "film cap ref 30/55/85 C"},
        {12, "C_res", Kind::Electrolytic, 30, 62, 14, 14, 20, {30, 55, 85}, "electrolytic ref 30/55/85 C"},
        {13, "D_rect", Kind::Diode, 10, 66, 12, 5, 4, {35, 80, 115}, "rectifier ref 35/80/115 C"},
        {14, "R_filt", Kind::Resistor, 52, 67, 14, 4.5, 4, {35, 80, 120}, "wirewound ref 35/80/120 C"},
        {15, "C_filt", Kind::Electrolytic, 74, 63, 12, 12, 16, {30, 55, 85}, "electrolytic ref 30/55/85 C"},
        {16, "Chassis", Kind::Chassis, 3, 3, 20, 14, 3, {30, 50, 70}, "chassis node ref 30/50/70 C"},
    }};
    return p;
}

const std::vector<Trace>& traces() {
    static const std::vector<Trace> t = {
        // Signal path: input pad -> C_in -> grid node -> V1 grid; plate -> C_out -> T_out -> output pad.
        {Net::Signal, false, {{2, 33}, {12, 33}}},
        {Net::Signal, false, {{24, 33}, {44, 33}}},
        {Net::Signal, false, {{26.25, 33}, {26.25, 42}}},
        {Net::Signal, false, {{62, 35}, {72.25, 35}, {72.25, 33}, {74, 33}}},
        {Net::Signal, false, {{72.25, 35}, {72.25, 23}}},
        {Net::Signal, false, {{86, 33}, {96, 33}}},
        {Net::Signal, false, {{116, 33}, {128, 33}}},
        // Cathode network to the ground bus.
        {Net::Signal, false, {{53, 44}, {53, 47}, {49.25, 47}, {49.25, 49}}},
        {Net::Signal, false, {{53, 47}, {60, 47}, {60, 49}}},
        {Net::Ground, false, {{26.25, 54}, {26.25, 58}}},
        {Net::Ground, false, {{49.25, 57}, {49.25, 58}}},
        {Net::Ground, false, {{60, 57}, {60, 58}}},
        {Net::Ground, false, {{4, 58}, {119, 58}}},
        // B+ rail bus and branches.
        {Net::Rail, false, {{28, 4}, {118, 4}}},
        {Net::Rail, false, {{72.25, 10}, {72.25, 4}}},
        {Net::Rail, false, {{84, 10.25}, {84, 4}}},
        {Net::Rail, false, {{96, 10.25}, {100, 10.25}}},
        {Net::Rail, false, {{105, 15}, {105, 22}}},
        {Net::Rail, false, {{28, 12.25}, {28, 4}}},
        // Screen supply (pentode only).
        {Net::Screen, false, {{40, 12.25}, {43, 12.25}, {43, 18.5}, {44, 18.5}}},
        {Net::Screen, false, {{54, 18.5}, {57, 18.5}, {57, 26}}},
        // Supply chain: AC pad -> rectifier -> reservoir -> CRC filter -> rail feed (bottom layer).
        {Net::Rail, false, {{3, 68.5}, {10, 68.5}}},
        {Net::Rail, false, {{22, 68.5}, {30, 68.5}}},
        {Net::Rail, false, {{44, 69}, {52, 69.25}}},
        {Net::Rail, false, {{66, 69.25}, {74, 69.25}}},
        {Net::Rail, true, {{86, 69}, {121, 69}, {121, 4}, {118, 4}}},
    };
    return t;
}

const std::vector<FlowPath>& flowPaths() {
    static const std::vector<FlowPath> f = {
        {FlowSource::InputLevel, {{2, 33}, {44, 33}}, "audio in"},
        {FlowSource::StageCurrent, {{72.25, 10}, {72.25, 35}}, "plate current"},
        {FlowSource::OutputLevel, {{86, 33}, {128, 33}}, "audio out"},
    };
    return f;
}

double thermalPosition(const ThermalRef& r, double t) noexcept {
    if (t <= r.coolBelow) return 0.0;
    if (t <= r.normalMax) return 0.5 * (t - r.coolBelow) / std::max(r.normalMax - r.coolBelow, 1e-9);
    if (t >= r.stress) return 1.0;
    return 0.5 + 0.5 * (t - r.normalMax) / std::max(r.stress - r.normalMax, 1e-9);
}

ThermalState thermalState(const ThermalRef& r, double t) noexcept {
    if (t < r.coolBelow) return ThermalState::Cool;
    if (t <= r.normalMax) return ThermalState::Normal;
    if (t < r.stress) return ThermalState::Warm;
    return ThermalState::Stress;
}

const char* thermalStateName(ThermalState s) noexcept {
    switch (s) {
        case ThermalState::Cool: return "Cool";
        case ThermalState::Normal: return "Normal";
        case ThermalState::Warm: return "Warm";
        case ThermalState::Stress: return "Stress";
    }
    return "";
}

// ---- Projection -------------------------------------------------------------------------------------
void Projection::rotated(double x, double y, double z, double& X, double& Y) const noexcept {
    const double u = x - kBoardW * 0.5, v = y - kBoardH * 0.5;
    const double c = std::cos(yaw_), s = std::sin(yaw_);
    X = c * u - s * v;
    const double Yr = s * u + c * v;
    Y = Yr * std::sin(pitch_) - z * std::cos(pitch_);
}

void Projection::refit() {
    double minX = 1e18, maxX = -1e18, minY = 1e18, maxY = -1e18;
    const double zs[2] = {-kBoardThickness, maxZ_};
    for (double z : zs)
        for (double x : {0.0, kBoardW})
            for (double y : {0.0, kBoardH}) {
                double X, Y;
                rotated(x, y, z, X, Y);
                minX = std::min(minX, X); maxX = std::max(maxX, X);
                minY = std::min(minY, Y); maxY = std::max(maxY, Y);
            }
    const double margin = 14;
    s0_ = std::max(0.05, std::min((vw_ - 2 * margin) / std::max(maxX - minX, 1e-6), (vh_ - 2 * margin) / std::max(maxY - minY, 1e-6)));
    ox_ = vw_ * 0.5 - s0_ * (minX + maxX) * 0.5;
    oy_ = vh_ * 0.5 - s0_ * (minY + maxY) * 0.5;
}

Pt Projection::project(double x, double y, double z) const noexcept {
    double X, Y;
    rotated(x, y, z, X, Y);
    const double px = s0_ * X + ox_, py = s0_ * Y + oy_;
    return {vw_ * 0.5 + zoom_ * (px - vw_ * 0.5) + panX_, vh_ * 0.5 + zoom_ * (py - vh_ * 0.5) + panY_};
}

double Projection::depth(double x, double y, double z) const noexcept {
    const double u = x - kBoardW * 0.5, v = y - kBoardH * 0.5;
    const double Yr = std::sin(yaw_) * u + std::cos(yaw_) * v;
    return Yr * std::cos(pitch_) + z * std::sin(pitch_);
}

void Projection::plane(double z, double& a, double& b, double& c, double& d, double& e, double& f) const noexcept {
    const Pt p0 = project(0, 0, z), p1 = project(1, 0, z), p2 = project(0, 1, z);
    a = p1.x - p0.x; b = p2.x - p0.x; c = p0.x;
    d = p1.y - p0.y; e = p2.y - p0.y; f = p0.y;
}

double Projection::faceFacing(double nx, double ny) const noexcept {
    const double Yr = std::sin(yaw_) * nx + std::cos(yaw_) * ny;
    return Yr * std::cos(pitch_);
}

Bounds projectedBounds(const Projection& p, const Part& part, double heightScale, double minSizePx) noexcept {
    Bounds b{1e18, 1e18, -1e18, -1e18};
    for (double z : {0.0, part.height * heightScale})
        for (double x : {part.x, part.x + part.w})
            for (double y : {part.y, part.y + part.h}) {
                const Pt s = p.project(x, y, z);
                b.x0 = std::min(b.x0, s.x); b.x1 = std::max(b.x1, s.x);
                b.y0 = std::min(b.y0, s.y); b.y1 = std::max(b.y1, s.y);
            }
    if (minSizePx > 0) {
        const double cx = (b.x0 + b.x1) * 0.5, cy = (b.y0 + b.y1) * 0.5;
        if (b.x1 - b.x0 < minSizePx) { b.x0 = cx - minSizePx * 0.5; b.x1 = cx + minSizePx * 0.5; }
        if (b.y1 - b.y0 < minSizePx) { b.y0 = cy - minSizePx * 0.5; b.y1 = cy + minSizePx * 0.5; }
    }
    return b;
}

std::vector<int> hitCandidates(const Projection& p, double heightScale, double px, double py, double minSizePx) {
    // Rank: parts whose true projected bounds contain the point first (nearest depth first), then
    // parts reached only through the >= 32 px expansion (closest centre first).
    struct Hit { int tier; double key; int id; };
    std::vector<Hit> hits;
    for (const auto& part : parts()) {
        const Bounds e = projectedBounds(p, part, heightScale, minSizePx);
        if (px < e.x0 || px > e.x1 || py < e.y0 || py > e.y1) continue;
        const Bounds t = projectedBounds(p, part, heightScale, 0);
        const bool exact = px >= t.x0 && px <= t.x1 && py >= t.y0 && py <= t.y1;
        if (exact) hits.push_back({0, -p.depth(part.x + part.w * 0.5, part.y + part.h * 0.5, part.height * heightScale), part.id});
        else hits.push_back({1, std::hypot(px - (t.x0 + t.x1) * 0.5, py - (t.y0 + t.y1) * 0.5), part.id});
    }
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.tier != b.tier ? a.tier < b.tier : a.key < b.key; });
    std::vector<int> ids;
    for (const auto& h : hits) ids.push_back(h.id);
    return ids;
}

}  // namespace cd::plugin::ui::pcb
