// CircuitDrift edition: LITE state document, upgrade export to a full scene, full-scene import and
// the LITE compatibility check (07-lite-edition "State and upgrade"; CD-F05 / T05).
// Owner: cd_edition. Uses cd::core JSON and cd::state StateDocument.
//
// LITE state JSON (strict: unknown keys, duplicate keys, non-finite or out-of-range values,
// another edition or a different liteMappingVersion are rejected, never clamped):
//   {"schemaVersion":1,"edition":"lite","liteMappingVersion":1,"sag":s,"thermalAge":a,
//    "seed":<integer 0..2^53>,"qualityRevision":1,"ui":{...}}
// ui holds local preferences (scale/theme/accessibility) and is round-tripped untouched.
// Upgrade export: a full scene (cd::state::StateDocument, kind Scene) with the fixed factory
// blueprint, fixed base controls, the exact mapped macro values (also in baseParameters macro.*),
// seed and engine model version, plus "liteUpgrade" provenance metadata. createdAt is supplied by
// the caller (the engine never reads wall time).
// Full import (sceneToEngineConfig): builds the full Engine configuration from a scene. Features
// this engine cannot render (analog noise, creative pitch delay, timeline/fault payloads, non-
// default values of unwired parameters) are rejected with a reason, never silently dropped.
// LITE import of a full scene (checkLiteCompatible): accepted only when the scene is exactly
// representable by LiteMapping v1; otherwise the full list of incompatibilities is returned.
// The seed is stored and exported; no seeded stochastic process exists in the engine yet.
// Threading: non-audio only (allocates).
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "core/Json.h"
#include "edition/EditionManifest.h"
#include "state/StateDocument.h"

namespace cd::edition {

inline constexpr int kLiteSchemaVersion = 1;

struct LiteState {
    LiteKnobs knobs{};
    std::uint64_t seed = 0;
    int qualityRevision = kLiteQualityRevision;
    core::JsonValue ui = core::JsonValue::makeObject();
};

bool saveLiteState(const LiteState& s, std::string& out, std::string& error);
bool loadLiteState(std::string_view text, LiteState& out, std::string& error);

// Full scene for the LITE state (exact mapped values). createdAt: RFC 3339 UTC.
bool exportLiteUpgrade(const LiteState& s, const std::string& createdAt, state::StateDocument& out, std::string& error);

struct SceneEngineConfig {
    PreparedEngineConfig prepared;
    std::uint64_t seed = 0;
};
// Full-edition import of a scene document into an Engine configuration.
bool sceneToEngineConfig(const state::StateDocument& doc, double fs, int maxBlock, int channels, SceneEngineConfig& out,
                         std::string& error);

struct LiteCompatibility {
    bool compatible = false;
    std::vector<std::string> issues;   // compatibility summary (empty when compatible)
    std::string summary() const;
};
// LITE import of a full scene. On success `out` holds the equivalent LITE state.
LiteCompatibility checkLiteCompatible(const state::StateDocument& doc, LiteState& out);

}  // namespace cd::edition
