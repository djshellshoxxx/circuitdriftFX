// CircuitDrift state: versioned .circuitdrift.json scene/session documents (P04, 05-state spec).
// Envelope schemaVersion=1; strict rejection (duplicate keys, non-finite, depth >20, size bounds,
// unknown required feature/model/parameter, out-of-range values, incompatible versions); no
// silent clamping. Migrations are pure ordered vN->vN+1 transforms on the parsed JSON tree.
// Optional engine checkpoint (session only) is version/model/Fs/OS-qualified; an incompatible
// checkpoint resets to warm initialisation and reports why. Cross-version/Fs checkpoint
// migration policy is G12 (blocked): no checkpoint is ever transformed.
// Threading: everything here is worker/UI-thread only (allocates, may do file I/O).
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "core/Json.h"

namespace cd::state {

constexpr int kSchemaVersion = 1;
constexpr int kReaderVersion = 1;
// Only the current synthetic model contract is supported; future model versions
// require an explicit configuration compatibility/migration decision.
inline constexpr std::string_view kEngineModelVersion = "0.0.1-dev";
constexpr std::size_t kSceneMaxBytes = 1310720;     // 1.25 MiB
constexpr std::size_t kSessionMaxBytes = 2621440;   // 2.5 MiB
constexpr std::size_t kBlueprintMaxBytes = 1048576; // 1 MiB
constexpr std::size_t kMetadataMaxBytes = 65536;    // preserved unknown metadata bound

enum class DocKind : unsigned char { Scene, Session };

struct EngineCheckpoint {
    std::string engineModelVersion, modelRevision;
    double sampleRate = 0;
    int oversampling = 1;
    std::uint64_t sampleIndex = 0;
    std::vector<double> electrical, thermal;
    std::vector<std::uint64_t> rng;
};

struct StateDocument {
    DocKind kind = DocKind::Scene;
    std::string engineModelVersion;
    int minReaderVersion = 1;
    std::string author, title, createdAt;
    std::vector<std::string> tags, requiredFeatures;
    std::vector<core::JsonValue> blueprints;      // scene: exactly 1; session: 1..2 (A/B)
    std::vector<std::pair<std::string, double>> baseParameters;  // registry id -> plain value
    core::JsonValue macros, timeline, faults;     // structure-checked only (P05/P11 own semantics)
    std::uint64_t seed = 0;
    std::optional<core::JsonValue> ui;            // local only; stripped on publish
    std::optional<EngineCheckpoint> checkpoint;   // session only; stripped on publish
    std::vector<std::pair<std::string, core::JsonValue>> metadata;  // unknown harmless top-level keys
};

// Migration step k transforms a document of version (chain.firstVersion + k) to the next version
// and must set schemaVersion accordingly. Pure: depends only on its input tree.
using MigrationFn = bool (*)(core::JsonValue& doc, std::string& error);
struct MigrationChain {
    int firstVersion = kSchemaVersion;            // oldest version the chain accepts
    std::vector<MigrationFn> steps;               // steps.size() == kSchemaVersion - firstVersion
};
// v1 is the first released schema, so the production chain is empty.
const MigrationChain& productionMigrations();

struct LoadReport { int sourceVersion = 0; int migrationsApplied = 0; };

bool loadState(std::string_view text, StateDocument& out, std::string& error,
               const MigrationChain& chain = productionMigrations(), LoadReport* report = nullptr);
// Numeric seed/sampleIndex identities are limited to [0, 2^53] by schema v1.
// Throws std::invalid_argument instead of rounding larger in-memory identities.
std::string saveState(const StateDocument& doc);
// Community/scene export: requires kind Scene; strips checkpoint, UI preferences and metadata.
// Until P05/P10/P11 provide complete validators, only the minimal synthetic blueprint
// {schemaVersion,id,components:[{id,modelId}]} and empty macros/timeline/faults are
// publishable. Other content is rejected, never silently removed from the sound.
// Failure leaves out unchanged. This serializes a scene; it does not upload anything.
bool publishScene(const StateDocument& doc, std::string& out, std::string& error);
// Uses an exclusively created, unique temporary file in the destination directory;
// checks write/flush/close before replacing the destination. No temporary symlink
// is followed. Existing destination symlinks are replaced, not followed.
// Requires a trusted destination directory. Failure before replacement preserves
// the old file; successful return does not promise directory durability after power loss.
// POSIX and Windows local-file replacement are supported; never call from audio.
bool saveStateFileAtomic(const std::string& path, const std::string& text, std::string& error);

struct RuntimeIdentity {
    std::string engineModelVersion, modelRevision;
    double sampleRate = 0;
    int oversampling = 1;
    std::size_t electricalSize = 0, thermalSize = 0;
};
enum class RestoreDecision : unsigned char { Restore, ResetWarm, NoCheckpoint };
RestoreDecision decideRestore(const StateDocument& doc, const RuntimeIdentity& rt, std::string& reason);

}  // namespace cd::state
