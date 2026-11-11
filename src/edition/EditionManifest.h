// CircuitDrift edition: edition manifests, format identities and the LITE two-knob mapping
// (P12 LITE; docs/planned-features/07-lite-edition.md; CD-F05 / T05).
// Owner: cd_edition. Depends on cd_runtime/cd_blueprint/cd_control/cd_core; never on UI/JUCE.
//
// LITE and full compile the SAME engine library; an edition differs only by this manifest:
//  * Exposed host parameters: LITE exposes exactly two primary sound controls, lite.sag (1040,
//    default 0.25) and lite.thermalAge (1041, default 0.2). Full exposes every registry ID except
//    the LITE-only ones. No hidden third tone control.
//  * LiteMapping v1 (liteMappingVersion = 1, all constants versioned together): Sag -> macro Sag
//    = s (full Sag macro); Thermal Age a -> macro Age = a, Drift = 0.25*a, creative pitch delay
//    disabled; Warmth fixed 0.2; Analog Noise off; mix 1; auto-gain on; input/output trims 0 dB;
//    HPF on at 15 Hz; soft clip on; quality 4x FIR Eco.
//  * Fixed clean factory topology: the SYNTHETIC fixture-v1 antiparallel diode clipper blueprint
//    below (labelled synthetic; not a hardware model, G01 open).
// The factory topology carries the reduced SYNTHETIC supply reservoir runtime::syntheticFactorySupply()
// (attached by circuitFromBlueprint to the canonical factory blueprint, so LITE, the equivalent full
// configuration and the upgrade-export import are identical). The Sag knob is therefore audible:
// reservoir C_eff = max(C_min, C_base/(1+3s)) and the Main-source load I_phys + 3 s I_dyn drive the
// rail-referenced headroom stage (base droop remains at s = 0). Honest remaining limits: no
// temperature law and no creative pitch delay on this topology, so Drift (0.25a) has no binding and
// stays neutral/unsupported; Thermal Age is audible through Age's creative -6*a dB 6 kHz shelf only.
// Transient preservation and the external sidechain default to off (LITE exposes no control).
// Format identities: PROPOSED PLACEHOLDERS ONLY (G08 AAX access/signing, G09 adapter pins and
// vendor registration). They are syntactically distinct strings under the reserved ".invalid"
// namespace; no vendor-registered code (AU manufacturer/subtype, AAX product ID, VST3 class ID)
// has been invented. Real identifiers must be assigned through the owner workflow, then frozen.
// Threading: everything here is immutable data or pure functions (any thread, no allocation in
// mapLite/exposes).
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include "control/Macros.h"
#include "runtime/Engine.h"

namespace cd::edition {

enum class Edition : std::uint8_t { Full, Lite };
enum class PluginFormat : std::uint8_t { Vst3, Au, Aax, Clap };
inline constexpr int kFormatCount = 4;

struct FormatIdentity {
    PluginFormat format;
    const char* pluginId;   // PROPOSED placeholder (VST3 class ID / AU subtype / AAX product / CLAP id)
    const char* bundleId;   // PROPOSED placeholder bundle identifier
};

struct EditionIdentity {
    Edition edition;
    const char* productName;
    std::array<FormatIdentity, kFormatCount> formats;
};

inline constexpr const char* kIdentityStatus =
    "PROPOSED placeholder identifiers pending G08 (AAX SDK/signing) and G09 (format adapter pins, vendor registration)";
const EditionIdentity& editionIdentity(Edition e) noexcept;
// True when every plugin and bundle identifier across both editions and all formats is unique.
bool identitiesDistinct(std::string& why);

// ---- manifests ----
inline constexpr std::uint32_t kLiteSagHostId = 1040, kLiteThermalAgeHostId = 1041;

struct EditionManifest {
    Edition edition;
    const char* name;
    std::span<const std::uint32_t> primaryControls;   // primary sound knobs shown on the main surface
    bool graphEditing, faults, ipc, communityPublishing, abDualEngines, mediaRecorder, workbench3d;
    int liteMappingVersion;                             // 0 for Full
};
const EditionManifest& editionManifest(Edition e) noexcept;
// Whether an edition exposes a host parameter (registry hostId) to the host.
bool exposes(Edition e, std::uint32_t hostId) noexcept;

// ---- LITE mapping v1 ----
inline constexpr int kLiteMappingVersion = 1;
inline constexpr double kLiteDefaultSag = 0.25, kLiteDefaultThermalAge = 0.2;
inline constexpr double kLiteWarmth = 0.2, kLiteDriftPerAge = 0.25, kLiteHpfHz = 15.0;
inline constexpr int kLiteQualityRevision = 1;   // "4x FIR Eco", revision 1

struct LiteKnobs { double sag = kLiteDefaultSag, thermalAge = kLiteDefaultThermalAge; };

struct LiteMapped {
    control::MacroValues macros{};
    runtime::OutputParams output{};
    runtime::QualitySettings quality{};
    bool analogNoise = false, pitchDelay = false;
};
// Pure. Knobs are clamped to [0,1]; non-finite values map to the defaults.
LiteMapped mapLite(const LiteKnobs& k) noexcept;

// Canonical JSON of the fixed clean factory topology (SYNTHETIC fixture-v1 diode clipper).
const char* liteFactoryBlueprintJson() noexcept;
// True when bp is canonically identical to the factory topology (non-audio). circuitFromBlueprint
// attaches runtime::syntheticFactorySupply() (reduced SYNTHETIC supply reservoir) to exactly this
// topology, in LITE and in full alike.
bool isFactoryTopology(const blueprint::Blueprint& bp);

// Engine configuration plus the compiled graph whose manifests the circuit netlist points into
// (keep `graph` alive until the Engine is prepared; CircuitRuntime deep-copies the manifests).
struct PreparedEngineConfig {
    runtime::EngineConfig engine;
    blueprint::PreparedHandle graph;
};
// Compiles `blueprintJson` for (fs, oversampling factor of `quality`) and fills cfg.circuit.
bool circuitFromBlueprint(std::string_view blueprintJson, double fs, int maxBlock, int channels,
                          const runtime::QualitySettings& quality, PreparedEngineConfig& cfg, std::string& error);
// LITE engine configuration for the given knobs (non-audio).
bool liteEngineConfig(const LiteKnobs& k, double fs, int maxBlock, int channels, PreparedEngineConfig& out, std::string& error);

}  // namespace cd::edition
