// CircuitDrift plugin UI: PCB model for the Workbench (CD-F17). Pure std C++ (no JUCE): board
// layout of the factory-circuit components (engine::componentTable keys), copper traces, signal
// arrow paths, per-component thermal references and the orthographic orbit projection.
// The layout is an illustrative board, not a claim about a real PCB. Board units are arbitrary
// (about 130 x 82); y grows downward, z is height above the substrate.
#pragma once
#include <array>
#include <cmath>
#include <vector>

namespace cd::plugin::ui::pcb {

constexpr int kPartCount = 17;  // == engine::kComponentCount (ComponentId order)
constexpr double kBoardW = 130.0, kBoardH = 82.0, kBoardThickness = 2.5;

enum class Kind { Resistor, FilmCap, Electrolytic, Tube, Transformer, Diode, Chassis };
struct Pt { double x = 0, y = 0; };

// Component-specific thermal references in deg C (D15: no universal limit). Illustrative synthetic
// values, shown to the user verbatim in the inspector/legend; never claimed as hardware limits.
struct ThermalRef { double coolBelow, normalMax, stress; };

struct Part {
    int id;                 // engine::ComponentId as int (also the index into pcbParts())
    const char* key;        // engine::ComponentInfo::key
    Kind kind;
    double x, y, w, h;      // footprint on the board
    double height;          // extrusion for the oblique view
    ThermalRef ref;
    const char* refBasis;   // short text describing the reference
};
const std::array<Part, kPartCount>& parts();

enum class Net { Signal, Rail, Ground, Screen };
struct Trace { Net net; bool bottomLayer; std::vector<Pt> pts; };
const std::vector<Trace>& traces();

// Signal-flow guides for the Signal overlay (direction = point order).
enum class FlowSource { InputLevel, OutputLevel, StageCurrent };
struct FlowPath { FlowSource source; std::vector<Pt> pts; const char* label; };
const std::vector<FlowPath>& flowPaths();

enum class ThermalState { Cool, Normal, Warm, Stress };
ThermalState thermalState(const ThermalRef& r, double tempC) noexcept;
// 0 at/below coolBelow, 0.5 at normalMax, 1 at/above stress (piecewise linear between anchors).
double thermalPosition(const ThermalRef& r, double tempC) noexcept;
const char* thermalStateName(ThermalState s) noexcept;

// Orthographic orbit camera. pitch = pi/2 is a straight top-down (2D board) view.
class Projection {
public:
    void setViewport(double w, double h) { vw_ = w; vh_ = h; refit(); }
    void setOrbit(double yawRad, double pitchRad) { yaw_ = yawRad; pitch_ = pitchRad; refit(); }
    void setExtrusion(double maxHeight) { maxZ_ = maxHeight; refit(); }
    void setZoom(double z) { zoom_ = z < 0.4 ? 0.4 : (z > 6.0 ? 6.0 : z); }
    void addPan(double dx, double dy) { panX_ += dx; panY_ += dy; }
    void resetView() { zoom_ = 1; panX_ = panY_ = 0; }
    double yaw() const noexcept { return yaw_; }
    double pitch() const noexcept { return pitch_; }
    double zoom() const noexcept { return zoom_; }
    Pt project(double x, double y, double z) const noexcept;
    // Larger = nearer to the viewer (painter's order / overlap resolution).
    double depth(double x, double y, double z) const noexcept;
    // Screen affine of the plane at height z: (u,v) in board units -> screen.
    void plane(double z, double& a, double& b, double& c, double& d, double& e, double& f) const noexcept;
    // Outward normal of a vertical face facing (nx,ny) is visible when this is > 0.
    double faceFacing(double nx, double ny) const noexcept;
private:
    void refit();
    void rotated(double x, double y, double z, double& X, double& Y) const noexcept;
    double yaw_ = 0, pitch_ = 1.5707963267948966, zoom_ = 1, panX_ = 0, panY_ = 0, vw_ = 100, vh_ = 100, maxZ_ = 0;
    double s0_ = 1, ox_ = 0, oy_ = 0;
};

// Screen-space bounds of a part (all 8 box corners), optionally expanded to a minimum hit size.
struct Bounds { double x0, y0, x1, y1; };
Bounds projectedBounds(const Projection& p, const Part& part, double heightScale, double minSizePx) noexcept;
// All parts whose (expanded) bounds contain the point, nearest first.
std::vector<int> hitCandidates(const Projection& p, double heightScale, double px, double py, double minSizePx);

}  // namespace cd::plugin::ui::pcb
