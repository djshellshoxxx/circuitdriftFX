// CircuitDrift state: scene presets and full-config session documents on top of the EXISTING
// StateDocument format (schemaVersion 1; no new document kind, no format change). Worker/message
// thread only.
//
// ScenePreset (kind "scene") = one circuit + its authored globals:
//   * the 39 slot params of ONE slot, stored under the slot.a.* ids (a scene is slot-neutral; the
//     loader applies it to the selected slot) + blueprint id + seed;
//   * kSceneGlobalCount authored globals (macros, analog-noise switch, mix, auto-gain, sidechain,
//     transient, harmonics). Not part of a scene: input/output gain, HPF, quality, key/tempo,
//     network, ab.crossfade, lite.* (host/session level).
// File layout (canonical writer): baseParameters lists ALL scene params in a fixed order; macros={},
// timeline={}, faults=[], blueprint={schemaVersion:1,id,components:[]} (opaque blueprint content
// belongs to P10 and is rejected, never dropped); seed = slot seed. requiredFeatures = CD-F20 plus the
// features of every non-default parameter, sorted. Strict reader: rejects unknown/duplicate keys (via
// the shared parser), keys outside the scene set, out-of-range / non-integral discrete / non-finite
// values (never clamped), non-empty macros/timeline/faults, blueprint extras, ui, checkpoint and
// unknown top-level metadata. Parameters absent from baseParameters take registry defaults; the
// canonical writer always emits all of them, so save -> load -> save is a byte-exact fixed point.
//
// Session documents (configToSessionJson) carry the whole Config: all registry params, two
// blueprints {schemaVersion,id,components:[],seed} (per-slot seeds), doc.seed == slot A seed.
// Doubles are written with 17 significant digits and parse back bit-exactly (T24 config roundtrip).
#pragma once
#include <array>
#include <string>
#include <string_view>
#include <vector>
#include "state/Config.h"
#include "state/StateDocument.h"

namespace cd::state {

inline constexpr std::array<std::string_view, 13> kSceneGlobalIds = {
    "macro.age", "macro.sag", "macro.drift", "macro.warmth", "analogNoise.enabled", "mix", "gainMatch.enabled",
    "sidechain.source", "sidechain.depth", "sidechain.hpf", "transient.preserve", "harmonics.target", "harmonics.amount"};
inline constexpr std::size_t kSceneGlobalCount = kSceneGlobalIds.size();

constexpr std::array<std::size_t, kSceneGlobalCount> sceneGlobalIndices() noexcept {
    std::array<std::size_t, kSceneGlobalCount> a{};
    for (std::size_t i = 0; i < kSceneGlobalCount; ++i) a[i] = core::indexOf(kSceneGlobalIds[i]);
    return a;
}
inline constexpr auto kSceneGlobalIndices = sceneGlobalIndices();

struct PresetMeta {
    std::string title, author;
    std::vector<std::string> tags;
    std::string createdAt = "2026-01-01T00:00:00Z";  // RFC 3339 UTC; supplied by the caller (no clock here)
};

struct ScenePreset {
    PresetMeta meta;
    SlotConfig slot;
    std::array<double, kSceneGlobalCount> globals{};
    ScenePreset() noexcept;  // registry defaults
    friend bool operator==(const ScenePreset& a, const ScenePreset& b) noexcept;
};

// Capture slot `slot` (0=A, 1=B) and the authored globals of `c`.
ScenePreset captureScene(const Config& c, int slot, PresetMeta meta);
// New config with the scene applied to `slot`. circuitOnly keeps the current authored globals
// ("Load circuit only": the authored sound may then differ -- the UI must warn).
Config applyScene(const Config& current, int slot, const ScenePreset& p, bool circuitOnly = false);
// True when `current` still equals the loaded scene on its scope (slot params, blueprint id, seed and,
// unless circuitOnly, the authored globals). UI view/theme/host-level params never dirty a preset.
bool matchesScene(const Config& current, int slot, const ScenePreset& p, bool circuitOnly = false);
inline bool isModified(const Config& current, int slot, const ScenePreset& p, bool circuitOnly = false) {
    return !matchesScene(current, slot, p, circuitOnly);
}

// Canonical scene JSON. Fails (never clamps) when any value/field violates the contract.
bool sceneToJson(const ScenePreset& p, std::string& out, std::string& error);
bool sceneFromJson(std::string_view text, ScenePreset& out, std::string& error);
bool sceneFromDocument(const StateDocument& doc, ScenePreset& out, std::string& error);

// Whole-config session document (T24 helper). `meta` supplies title/author/tags/createdAt.
bool configToSessionJson(const Config& c, const PresetMeta& meta, std::string& out, std::string& error);
bool configFromSessionJson(std::string_view text, Config& out, std::string& error);

// Factory presets compiled into the binary (synthetic/creative; not measurements of real hardware).
struct FactoryPreset {
    std::string id;  // stable slug, e.g. "clean-console"
    ScenePreset scene;
};
const std::vector<FactoryPreset>& factoryPresets();

}  // namespace cd::state
