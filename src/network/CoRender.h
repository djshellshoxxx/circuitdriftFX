// CircuitDrift network: deterministic co-render harness and influence-bundle replay (CD-F11, P16).
// Owner: cd_network. coRender()/bundle (de)serialization are NON-AUDIO (allocate);
// ReplaySource::bleedFor/railAt are audio-safe (noexcept, no allocation, bounded copies).
//
// coRender processes every participating dry stem against one common clock: per 128-frame
// quantum it evaluates every member's demand and ONE shared PSU solve in fixed member-ID order
// (ascending UUID bytes), then every receiver's bleed (CrossTalk.h, same arithmetic as the live
// broker, no ramps, no network delay), and renders each member's cd::runtime::Engine on
// stem + received bleed (bleed is added before the circuit; the tap is the dry stem, i.e. the
// signal after a 0 dB input trim). The group rail and every peer-bleed stream are recorded in a
// versioned InfluenceBundle together with a manifest: sample rate/start/length, topology (gain,
// seed, member order, channel layout, matrix hash), model versions (engine snapshot identity),
// PSU manifest hash and per-input content/automation hashes.
// Demand model (SYNTHETIC, labelled): P_j = powerW_j * (0.2 + 0.8 * rms activity), I_j = P_j / V;
// the demand depends on the rail, so V is solved by fixed-point iteration bounded to 8 iterations
// and an unconverged quantum rejects the whole render. The shared rail has no consumer in the
// synthetic circuits yet (no supply input in cd_runtime), so it is recorded and replayed as data.
// Replay: a ReplaySource binds one member to a bundle only if the bundle integrity hash, member
// routing (id present), content hash, automation hash, engine identity and sample rate all match;
// it serves bleed by ABSOLUTE sample index, so host callback order and block partition do not
// matter. Samples outside [start, start+length) get zero bleed and an out-of-range flag.
// Hashes are CDH64 (non-cryptographic; identity/corruption detection only, see Protocol.h).
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "network/CrossTalk.h"
#include "network/Protocol.h"
#include "runtime/Engine.h"

namespace cd::network {

using Stem = std::vector<std::vector<double>>;   // [channel][sample], sample 0 = render start

struct CoRenderMember {
    Uuid id{};
    runtime::EngineConfig engine{};   // channels must equal stem channels; sampleRate must match
    Stem stem;
    double powerW = 20.0;             // synthetic demand scale
};

struct CoRenderConfig {
    double sampleRate = 48000;
    std::int64_t start = 0;           // absolute timeline sample of stem index 0 (>= 0)
    std::int64_t length = 0;          // samples
    BleedConfig bleed{};
    std::uint64_t bleedSeed = 1;
    PsuManifest psu{};
    bool sharedDemand = true;
    bool useTopology = false;         // [receiver][source] in member-ID order
    std::vector<std::vector<double>> topology;
    int maxIterations = 8;            // fixed-point bound (<= 8)
};

struct InfluenceBundle {
    static constexpr std::uint32_t kVersion = 1;
    static constexpr const char* kModelVersion = "cd.network.corender-v1+synthetic-demand-v1";
    struct Member {
        Uuid id{};
        std::uint32_t channels = 0;
        std::uint64_t contentHash = 0, automationHash = 0, engineIdentityHash = 0;
        double powerW = 0;
        Stem bleed;                   // [channel][length]
    };
    double sampleRate = 0;
    std::int64_t start = 0, length = 0;
    double gain = 0, nominalV = 0;
    std::uint64_t bleedSeed = 0, psuHash = 0, topologyHash = 0;
    bool sharedDemand = true, rowClamped = false;
    std::vector<Member> members;      // fixed member-ID order
    std::vector<double> rail;         // per quantum [V]
    std::vector<std::uint8_t> railIterations;   // fixed-point iterations per quantum
    std::uint64_t integrityHash = 0;  // hash over manifest + streams (set by seal())

    std::uint64_t computeHash() const noexcept;
    void seal() noexcept { integrityHash = computeHash(); }
    std::vector<std::uint8_t> serialize() const;
    // Hostile input: bounds-checked, exact size, version and integrity hash verified.
    static bool parse(std::span<const std::uint8_t> bytes, InfluenceBundle& out, std::string& error);
};

struct CoRenderResult {
    InfluenceBundle bundle;
    std::vector<Stem> reference;      // [member (bundle order)] engine output of the co-render
    int maxIterationsUsed = 0;
};

std::uint64_t stemContentHash(const Stem& stem) noexcept;
std::uint64_t automationHash() noexcept;   // no automation lanes in this harness yet (hash of empty list)
std::uint64_t engineIdentityHash(const runtime::Engine& prepared) noexcept;

// Deterministic co-render (non-audio). False + error on invalid input or an unconverged solve.
bool coRender(const std::vector<CoRenderMember>& members, const CoRenderConfig& cfg, CoRenderResult& out,
              std::string& error);

class ReplaySource {
public:
    static constexpr std::uint32_t kOutOfRange = 1u << 0;
    // Non-audio. `bundle` must outlive this object.
    bool bind(const InfluenceBundle& bundle, const Uuid& member, std::uint64_t contentHash, std::uint64_t automation,
              std::uint64_t engineIdentity, double sampleRate, std::string& error);
    // Audio-safe. out: channels() pointers of `frames` samples. Returns flags.
    std::uint32_t bleedFor(std::int64_t sampleStart, std::uint32_t frames, double* const* out) const noexcept;
    double railAt(std::int64_t sample) const noexcept;   // nominal outside the range
    std::uint32_t channels() const noexcept { return m_ ? m_->channels : 0; }
    bool bound() const noexcept { return m_ != nullptr; }
private:
    const InfluenceBundle* b_ = nullptr;
    const InfluenceBundle::Member* m_ = nullptr;
    double nominal_ = 0;
};

}  // namespace cd::network
