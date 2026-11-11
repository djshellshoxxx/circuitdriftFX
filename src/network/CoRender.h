// CircuitDrift network: deterministic co-render harness and influence bundle replay (P16).
// Owner: cd_network. Spec 04-instance-network "Deterministic render mode" (D17).
// CoRenderSession processes every participating member's PRE-CIRCUIT dry stem and automation
// against one common clock: per 128-frame quantum (relative to startSample) it evaluates all
// member demands and ONE shared PSU solve in fixed member-UUID order, then mixes the bleed law
// (Bleed.h, zero latency on this path). Host "callbacks" may deliver member frames in any order
// and block partition (feed()); a quantum is processed only when every member has delivered it,
// so the output is bit-identical for every order/partition.
// Demand depends on the rail (SYNTHETIC demand model, labelled; not a validated circuit):
//     I_i(r) = (quiescentA_i + signalA_i * activity_i[q] * rms_i[q]) * r
// solved jointly with the shared reservoir by a fixed-point iteration on r = F(r) (F = one PSU
// trial solve of the summed demand), secant-accelerated after the first plain step, bounded to
// maxIterations (<= 8) evaluations and converged when |F(r) - r| <= tolerance; an unconverged
// quantum rejects the whole render (no partial bundle).
// The versioned InfluenceBundle records the group rail per quantum plus every member's bleed
// stream and a manifest: Fs/start/length, member order/channels, topology digest, model versions
// and model-parameter digest, seeds, and per-input content/automation SHA-256 (src/community/
// Sha256.h; doubles hashed as explicit little-endian IEEE-754 bytes). replay() re-renders and
// requires an identical manifest (any hash/routing/start/model/seed mismatch invalidates it with
// a reason) and streams within 1e-6 peak error. BundleReader serves one member's recorded
// streams to a later plugin render (open() checks that member's own digests; read() is
// audio-safe). Exact coupled co-render may be slower than realtime; this is the offline path,
// live IPC coupling stays "approximate live coupling".
// Threading: all non-audio except BundleReader::read (noexcept, allocation free).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "community/Sha256.h"
#include "network/Bleed.h"
#include "network/Protocol.h"
#include "network/SharedPsu.h"

namespace cd::network {

inline constexpr std::uint32_t kInfluenceBundleVersion = 1;
inline constexpr char kBleedModelVersion[] = "cd.network.bleed/1";
inline constexpr char kDemandModelVersion[] = "cd.network.demand.synthetic-rail-proportional/1";
inline constexpr std::uint32_t kMaxFixedPointIterations = 8;
inline constexpr double kReplayPeakTolerance = 1e-6;

using Digest = std::array<std::uint8_t, 32>;

struct CoRenderMemberInput {
    Uuid128 member;
    std::uint32_t channels = 2;
    std::vector<std::vector<double>> audio;   // [channel][lengthSamples] pre-circuit dry stem
    std::vector<double> activity;             // automation, one value per quantum (empty -> 1)
    double quiescentA = 0.05;                 // SYNTHETIC demand model parameters
    double signalA = 0.20;
    bool bleedOptIn = true;
};

struct CoRenderSpec {
    double sampleRate = 48000;
    std::uint64_t startSample = 0;
    std::uint64_t lengthSamples = 0;
    PsuSpec psu;
    BleedConfig bleed;
    std::vector<CoRenderMemberInput> members;
    std::uint32_t maxIterations = kMaxFixedPointIterations;
    double tolerance = 1e-10;           // |r_{n+1} - r_n| convergence bound
};

struct InputDigest {
    Uuid128 member;
    std::uint32_t channels = 0;
    Digest content{};
    Digest automation{};
};

struct BundleManifest {
    std::uint32_t formatVersion = kInfluenceBundleVersion;
    double sampleRate = 0;
    std::uint64_t startSample = 0;
    std::uint64_t lengthSamples = 0;
    std::uint32_t quantum = kQuantumFrames;
    std::vector<Uuid128> memberOrder;          // fixed member-UUID order
    std::vector<std::uint32_t> channels;
    Digest topology{};                         // order, channels, opt-ins, bleed level/matrix
    std::string psuModelVersion = kPsuModelVersion;
    std::string bleedModelVersion = kBleedModelVersion;
    std::string demandModelVersion = kDemandModelVersion;
    Digest modelParameters{};                  // PSU spec, demand params, iteration bound/tol
    std::uint64_t bleedSeed = 0;
    std::vector<InputDigest> inputs;           // in memberOrder
};

struct InfluenceBundle {
    BundleManifest manifest;
    std::vector<double> railFraction;          // per quantum (end of quantum)
    std::vector<std::uint8_t> iterations;      // fixed-point iterations used per quantum
    std::vector<std::vector<std::vector<double>>> bleed;   // [order index][channel][sample]
};

enum class CoRenderError : std::uint32_t {
    None = 0, InvalidSpec, DuplicateMember, InvalidBleed, InvalidPsu, BadFeed, IncompleteFeed, Unconverged
};
const char* toString(CoRenderError e) noexcept;

struct CoRenderResult {
    CoRenderError error = CoRenderError::None;
    std::string reason;
    InfluenceBundle bundle;
    std::uint32_t maxIterationsUsed = 0;
    bool ok() const noexcept { return error == CoRenderError::None; }
};

class CoRenderSession {
public:
    explicit CoRenderSession(const CoRenderSpec& spec);
    CoRenderError error() const noexcept { return error_; }
    const std::string& reason() const noexcept { return reason_; }
    // One host callback: `frames` frames of the member at spec index `memberIndex`, channels x frames.
    bool feed(std::size_t memberIndex, const double* const* data, std::size_t frames);
    CoRenderResult finish();

private:
    void fail(CoRenderError e, std::string why);
    void processReady();
    bool processQuantum(std::uint64_t q);

    CoRenderSpec spec_;   // audio vectors are not used by the session (data comes via feed)
    CoRenderError error_ = CoRenderError::None;
    std::string reason_;
    std::vector<std::size_t> order_;                       // order index -> spec index
    std::vector<std::size_t> orderOf_;                     // spec index -> order index
    std::vector<std::vector<std::vector<double>>> staged_; // [order][ch][sample]
    std::vector<std::uint64_t> delivered_;                 // per order index
    std::vector<std::vector<community::Sha256>> hashers_;  // [order][ch]
    std::uint64_t nextQuantum_ = 0;
    std::uint64_t quanta_ = 0;
    std::vector<double> W_;
    std::vector<BleedSource> sources_;
    std::vector<BleedSink> sinks_;
    ReservoirPsu psu_;
    std::uint32_t maxIterUsed_ = 0;
    InfluenceBundle bundle_;
};

struct FeedStep {
    std::size_t member = 0;   // spec index
    std::size_t frames = 0;
};
// Feeds each member's spec audio following `plan` (then any remainder sequentially) and finishes.
CoRenderResult coRender(const CoRenderSpec& spec, std::span<const FeedStep> plan = {});

enum class ReplayError : std::uint32_t {
    None = 0, FormatVersion, SampleRateMismatch, StartMismatch, LengthMismatch, RoutingMismatch,
    ModelMismatch, SeedMismatch, ContentHashMismatch, AutomationHashMismatch, UnknownMember,
    RenderFailed, StreamMismatch
};
const char* toString(ReplayError e) noexcept;

struct ReplayResult {
    ReplayError error = ReplayError::None;
    std::string reason;
    double peakError = 0;
    bool valid() const noexcept { return error == ReplayError::None; }
};

ReplayResult compareManifests(const BundleManifest& reference, const BundleManifest& actual);
// Re-renders `spec` (with `plan`) and checks it against the reference bundle.
ReplayResult replay(const InfluenceBundle& reference, const CoRenderSpec& spec, std::span<const FeedStep> plan = {});
// SHA-256 of one input as the session computes it (for BundleReader::open).
InputDigest digestInput(const CoRenderMemberInput& in, std::uint64_t lengthSamples);

class BundleReader {
public:
    // Non-audio: binds `own.member`'s streams if Fs/start match and its digests equal the manifest.
    ReplayResult open(const InfluenceBundle& bundle, const InputDigest& own, double sampleRate, std::uint64_t startSample);
    bool isOpen() const noexcept { return bundle_ != nullptr; }
    // Audio-safe: recorded bleed for [absoluteSample, absoluteSample + frames) (zeros outside the
    // bundle) and the rail fraction of the quantum containing absoluteSample (1 outside).
    double read(std::uint64_t absoluteSample, std::uint32_t frames, double* const* out, std::uint32_t channels) const noexcept;

private:
    const InfluenceBundle* bundle_ = nullptr;
    std::size_t index_ = 0;
};

}  // namespace cd::network
