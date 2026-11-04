// CircuitDrift uimodel: UX-01 PCB direct-manipulation gesture controller (pure mapping table).
// Owner: cd_uimodel. Framework-independent; message thread only (allocates). No audio access.
// Mapping (08-ux "UX-01"): tube bias drag -dyLogical/200 (Shift x0.1); pinch C=C0*r^2,
// Sag delta=-log2(r)/4 with r clamped 0.5..2 and results clamped to manifest/0..1; age brush
// signed pathLength/400 (Alt reverses) on a target set frozen at pointer-down, also moving the
// linked excess-noise modifier; material enum applied once; numeric field parse (units/range)
// with no state change on error; arrows +-1% / Shift +-0.1% of the normalized range; Home resets
// the factory base. Every continuous gesture is ONE host edit transaction (begin/perform/end),
// targets are published at <= 120 Hz (injected microsecond clock; the terminal value is always
// published once at end), Escape restores the start values exactly and records nothing, and a
// completed gesture is ONE cd::state::History entry. Pointer loss commits the last valid value.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "core/ParameterRegistry.h"
#include "state/History.h"
#include "uimodel/Document.h"

namespace cd::uimodel {

// Host-edit sink (wrapper adapter): one begin/perform*/end per target per gesture.
class EditHost {
public:
    virtual ~EditHost() = default;
    virtual void beginEdit(const std::string& id) = 0;
    virtual void performEdit(const std::string& id, double plainValue) = 0;
    virtual void endEdit(const std::string& id) = 0;
};

struct Point { double x = 0, y = 0; };

// ---- numeric entry ----
struct NumericResult {
    bool ok = false;
    double value = 0;       // plain units of the spec
    std::string error;      // human-readable, non-empty when !ok
};
// Parses "12.5", "12.5 dB", "1.5k", "1.5 kHz", "40%" (normalized only), "on/off" (bool).
NumericResult parseNumeric(const std::string& text, double min, double max, const std::string& unit, bool discrete);
NumericResult parseNumeric(const std::string& text, const core::ParamSpec& spec);

enum class GestureKind : std::uint8_t { None, Bias, Pinch, AgeBrush };

struct GestureConfig {
    double uiScale = 1.0;                        // 0.75..2 (clamped); dyLogical = dy / uiScale once
    std::string biasId = "tube.bias";            // normalized 0..1 entry
    std::string supplyCapId = "supply.capacitance";  // farads entry (manifest bounds = entry range)
    std::string sagId = "macro.sag";
    std::int64_t minPublishIntervalUs = 8334;    // ceil(1e6/120)
};

class GestureController {
public:
    using ClockUs = std::function<std::int64_t()>;
    GestureController(ParamDocument& doc, state::History& history, EditHost& host, ClockUs clock, GestureConfig cfg = {});

    bool active() const noexcept { return kind_ != GestureKind::None; }
    GestureKind kind() const noexcept { return kind_; }

    // Tube / Bias vertical drag. Returns false when a gesture is active or the target is unknown.
    bool beginBias(Point p);
    void moveBias(Point p, bool shift);
    // Supply pinch (two registered pointers required; distance must be > 0).
    bool beginPinch(Point a, Point b);
    void movePinch(Point a, Point b);
    // Age brush: ids frozen now; Alt reverses. Unknown ids are dropped; empty set -> false.
    bool beginBrush(Point p, const std::vector<std::string>& ids, bool alt);
    void moveBrush(Point p);
    const std::vector<std::string>& brushTargets() const noexcept { return brushIds_; }

    bool end();            // pointer up: publish final, end host edits, one history entry
    bool pointerLost();    // commit last valid value (same as end)
    bool cancel();         // Escape: restore start values, end host edits, no history entry

    // Discrete actions (each one transaction; refuse while a gesture is active).
    bool selectMaterial(const std::string& id, int materialIndex);  // clamps to the entry's enum range, no interpolation
    bool enterNumeric(const std::string& id, const std::string& text, std::string* error = nullptr);
    bool step(const std::string& id, int direction, bool shift);   // +-1% / +-0.1% of the range
    bool resetToFactory(const std::string& id);                    // Home

    std::int64_t publishCount() const noexcept { return publishes_; }
    std::int64_t gestureCount() const noexcept { return gestures_; }
    // Target ids (including linked) touched by the last/active gesture.
    const std::vector<std::string>& touched() const noexcept { return ids_; }

private:
    struct Saved { std::string id; double start; };
    bool open(GestureKind k, std::vector<std::string> ids);
    void publish(bool force);
    bool finish(bool commit);
    bool oneShot(const std::string& label, const std::vector<std::pair<std::string, double>>& changes);
    ParamDocument& doc_;
    state::History& hist_;
    EditHost& host_;
    ClockUs clock_;
    GestureConfig cfg_;
    GestureKind kind_ = GestureKind::None;
    std::vector<std::string> ids_;
    std::vector<double> start_;
    bool dirty_ = false;
    std::int64_t lastPublishUs_ = 0, publishes_ = 0, gestures_ = 0;
    bool havePublished_ = false;
    // gesture-specific
    Point last_;
    double raw_ = 0;           // bias: unclamped accumulator
    double d0_ = 1;            // pinch start distance
    double path_ = 0;          // brush: logical path length
    bool alt_ = false;
    std::vector<std::string> brushIds_;
};

std::string noiseLinkId(const std::string& componentId);   // "excessNoise." + id
std::string ageId(const std::string& componentId);         // "age." + id

}  // namespace cd::uimodel
