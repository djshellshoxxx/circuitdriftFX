// CircuitDrift network: deterministic co-render and influence bundle replay (P16, 04 spec, gate G05).
// The harness processes all members' dry stems against a common clock, in fixed member-ID order
// per 128-sample quantum, solves the shared rail jointly (fixed-point iteration, <= 8 iterations,
// unconverged renders are rejected) and records, per tick: group rail demand/voltage and each
// member's received bleed (computed from peers' dry audio at tick - latencyQuanta, zero before).
// Replay reads those exact streams by absolute sample position, so the result is independent of
// callback order and host block partition. The manifest (sample rate/start/length, topology,
// model version, seeds, per-input content hashes) must match or replay is refused.
// Integrity hash: FNV-1a 64 over the serialized bytes. It detects corruption/mismatch, it is not
// a cryptographic commitment. Worker/offline only (allocates).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "network/InstanceNetwork.h"

namespace cd::network {

constexpr std::uint32_t kBundleVersion = 1;
constexpr std::size_t kBundleMaxBytes = 256u * 1024u * 1024u;

std::uint64_t fnv1a64(const void* data, std::size_t n, std::uint64_t seed = 0xCBF29CE484222325ull) noexcept;
std::uint64_t hashSamples(const float* x, std::size_t n) noexcept;  // content hash for stems/automation

struct BundleManifest {
    std::uint32_t version = kBundleVersion;
    double sampleRate = 48000;
    std::uint64_t startSample = 0, lengthSamples = 0;
    std::uint32_t latencyQuanta = kLiveLatencyQuanta;
    std::uint64_t bleedSeed = 0;
    double bleedGainLinear = 0;
    double nominalRailV = 0, sourceOhms = 0;
    std::string modelVersion;
    std::vector<Uuid128> memberIds;            // fixed processing order (sorted ascending)
    std::vector<std::uint64_t> inputHashes;    // per member content/automation hash, same order
    friend bool operator==(const BundleManifest&, const BundleManifest&) = default;
};

struct InfluenceBundle {
    BundleManifest manifest;
    std::vector<float> railVolts, railDemandA;  // per tick
    std::vector<float> bleed;                   // [member][tick][channel][128]
    std::uint64_t ticks() const noexcept {
        return manifest.lengthSamples / kQuantumFrames + (manifest.lengthSamples % kQuantumFrames != 0);
    }
    std::size_t bleedOffset(std::size_t member, std::uint64_t tick, unsigned ch) const noexcept {
        return ((member * std::size_t(ticks()) + std::size_t(tick)) * kAudioChannels + ch) * kQuantumFrames;
    }
};

struct MemberStem {
    Uuid128 id;
    std::uint64_t contentHash = 0;
    std::vector<float> left, right;  // equal length >= manifest length
};

// demand(memberIndex, tick, railVolts) -> supply current [A]; must be a pure function of its inputs.
using DemandFn = std::function<double(std::size_t member, std::uint64_t tick, double railVolts)>;

struct CoRenderConfig {
    double sampleRate = 48000;
    std::uint64_t startSample = 0, lengthSamples = 0;
    std::uint32_t latencyQuanta = kLiveLatencyQuanta;
    std::uint64_t bleedSeed = 0;
    double bleedDb = -40.0;  bool bleedEnabled = true;
    double nominalRailV = 300.0, sourceOhms = 50.0;
    std::string modelVersion = "0.0.1-dev";
    std::vector<std::vector<float>> topology;  // optional NxN rows in sorted member order; empty = default
    int maxIterations = 8;
    double tolerance = 1.0e-6;                 // volts
};

bool coRender(const CoRenderConfig& cfg, std::vector<MemberStem> stems, const DemandFn& demand, InfluenceBundle& out, std::string& error);

// Returns empty for invalid manifest, dimensions or non-finite payload values.
std::vector<std::uint8_t> serializeBundle(const InfluenceBundle& b);
bool parseBundle(const std::uint8_t* data, std::size_t n, InfluenceBundle& out, std::string& error);

class BundleReplayer {
public:
    // Validates the bundle and refuses when its manifest differs from `expected` in any field.
    // The caller keeps `bundle` alive and immutable until the replayer is no longer used.
    bool open(const InfluenceBundle& bundle, const BundleManifest& expected, std::string& error);
    // Reads n samples starting at absolute sample position `pos` for member index `member`.
    // Out-of-range positions are an error (no extrapolation).
    bool readBleed(std::size_t member, std::uint64_t pos, std::size_t n, float* left, float* right) const;
    bool readRail(std::uint64_t pos, std::size_t n, float* volts, float* demandA) const;
private:
    const InfluenceBundle* b_ = nullptr;
};

}  // namespace cd::network
