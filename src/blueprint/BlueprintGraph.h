// CircuitDrift blueprint: typed declarative circuit graph, bounded parse + structural validation
// (03-blueprints spec, compiler check 1: "JSON schema and resource bounds"; schema
// docs/planned-features/schemas/blueprint.schema.json). Blueprints are data only: any executable
// or resource-reference field (script, shader, url, path, plugin, spice ...) is rejected.
// Optional extension beyond the published schema: top-level "modSlots" (16 Mod Slot bindings).
// Owner module: cd_blueprint. Threading: worker/UI thread only (allocates); never on audio.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "core/Json.h"

namespace cd::blueprint {

// Hard limits (spec "Hard limits").
inline constexpr std::size_t kMaxBytes = 1048576;  // 1 MiB UTF-8
inline constexpr int kMaxDepth = 16;
inline constexpr int kMaxComponents = 64;
inline constexpr int kMaxNodes = 33;               // including ground "0"
inline constexpr int kMaxPinConnections = 256;
inline constexpr int kMaxThermalNodes = 64;
inline constexpr int kMaxNonlinear = 8;
inline constexpr int kMaxUnknowns = 96;
inline constexpr int kMaxIdChars = 64;
inline constexpr int kMaxNameCodePoints = 80;
inline constexpr int kModSlotCount = 16;
inline constexpr int kMaxPinsPerComponent = 8;
inline constexpr int kMaxParamsPerComponent = 32;
inline constexpr int kMaxMacroBindings = 64;

enum class DiagCode : unsigned char {
    // check 1 (schema / bounds)
    Malformed, TooLarge, TooDeep, DuplicateKey, NonFinite, UnknownSchema, UnknownField, ExecutableField,
    ResourceReference, BadType, BadId, BadName, DuplicateId, LimitExceeded,
    // check 2 (models)
    RegistryMismatch, UnsupportedModel, PinMissing, PinUnknown, PinNode, ParamMissing, ParamUnknown,
    ParamRange, ConductanceUnbounded, BadBinding,
    // check 3 (connectivity)
    NoGround, BadTerminal, SelfShort, FloatingNode, FloatingIsland, NoDcPath, VoltageSourceLoop, SupplyShort,
    // checks 4-6
    BadConfig, UnknownCount, DcFailed, ProbeFailed, Internal
};
const char* diagCodeName(DiagCode c) noexcept;

struct Diagnostic {
    DiagCode code = DiagCode::Malformed;
    int check = 1;                 // 1..6, the spec compiler check that produced it
    std::string component, pin;    // empty when not applicable
    std::string message;
};

struct Diagnostics {
    static constexpr std::size_t kMax = 64;  // bounded; further items only set truncated
    std::vector<Diagnostic> items;
    bool truncated = false;
    void add(DiagCode code, int check, std::string component, std::string pin, std::string message);
    bool empty() const noexcept { return items.empty(); }
    bool has(DiagCode c) const noexcept;
    bool hasFor(DiagCode c, std::string_view component) const noexcept;
};

struct BpComponent {
    std::string id, modelId;
    std::vector<std::pair<std::string, std::string>> pins;     // pin name -> node id (JSON order)
    std::vector<std::pair<std::string, double>> params;        // parameter name -> value
    bool hasPosition = false;
    double position[3] = {0, 0, 0};                            // display only
    const std::string* pinNode(std::string_view pin) const noexcept;
    const double* param(std::string_view name) const noexcept;
};

struct MacroBinding { std::string macro, component, field; };
struct ModSlotBinding { int slot = 0; std::string component, field; };

struct Blueprint {
    int schemaVersion = 1;
    std::string id, name, modelRegistryVersion;
    std::vector<std::string> nodes;                // includes ground "0"
    std::vector<BpComponent> components;
    std::string input, output;                     // port nodes; ground is always "0"
    std::vector<MacroBinding> macroBindings;
    std::vector<ModSlotBinding> modSlots;          // stored in the blueprint, never host IDs
    int componentIndex(std::string_view id) const noexcept;
    int pinConnectionCount() const noexcept;
};

// Parse UTF-8 JSON text. Returns true only when no diagnostics were produced; out is then valid.
bool parseBlueprint(std::string_view text, Blueprint& out, Diagnostics& diag);
// Same for an already parsed tree (e.g. StateDocument::blueprints[i]). Re-applies the 1 MiB and
// depth bounds to the serialised/parsed tree.
bool parseBlueprint(const core::JsonValue& v, Blueprint& out, Diagnostics& diag);
// Canonical serialisation (round-trips through parseBlueprint). Throws only for non-finite numbers.
std::string writeBlueprint(const Blueprint& bp);

bool validIdentifier(std::string_view s) noexcept;       // 1..64 of [A-Za-z0-9_.-], not all dots
std::size_t utf8CodePoints(std::string_view s) noexcept;

}  // namespace cd::blueprint
