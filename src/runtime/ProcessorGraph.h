// CircuitDrift runtime: blueprint graph processors and their worker-side installer (resume packet 1,
// 03-blueprints "Apply only a successfully prepared immutable graph generation, at a callback
// boundary, with 20 ms crossfade ... Retire old memory after acknowledgment ... maximum of four
// prepared processors; coalesce pending Apply requests").
// Owner: cd_runtime.
//
// GraphProcessor: a CircuitRuntime prepared off-audio from a compiled cd::blueprint PreparedGraph
// for one engine configuration (Fs, channels, slice size, oversampling, Newton cap, padding). The
// graph's manifests are deep-copied by CircuitRuntime, so the PreparedGraph may be released after
// prepare. SYNTHETIC I/O contract: source volts = input * RuntimeConfig::fullScaleVolts on the
// compiler-injected drive source; output = V(ports.output) / fullScaleVolts (G01 open).
//
// GraphInstaller: worker-thread object owning a cd::state::GenerationPool of kSlots (=2) processor
// slots. submit() records the newest compiled graph (an older pending one is coalesced away) and
// service() publishes it when a slot whose processor the audio thread has released is available.
// Per Engine at most 2 graph processors exist (active + incoming/fading), so an A/B SlotEngine
// holds at most 4. The audio side (Engine, friend) installs the newest publish at a callback
// boundary, crossfades 20 ms, and then raises releasedBelow(); service() destroys a processor
// only when its generation is below that watermark (never on the audio thread). Pool reclamation
// alone (generation < acknowledged) is NOT sufficient for destruction because the previous graph
// is still fading out when the new one is acknowledged.
// Stale generations (0 or <= the last submitted) are rejected; a failed processor build leaves the
// pool and the audio path untouched.
// Threading: GraphProcessor::prepare, GraphInstaller::submit/service/stats: one worker thread.
// The Engine reads the pool and stores releasedBelow on the audio thread (lock/allocation free).
// The installer must outlive every Engine it is attached to (detach first).
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include "blueprint/Compiler.h"
#include "dsp/Oversampling.h"
#include "runtime/CircuitRuntime.h"
#include "state/TransactionQueue.h"

namespace cd::runtime {

// Engine configuration a graph processor must match (produced by Engine::graphTarget()).
struct GraphTarget {
    double sampleRate = 0;
    int maxBlock = 0, channels = 0;
    dsp::Oversampler::Spec os{};
    int maxNewton = 8;
    bool padToFamilyMax = true;
    int circuitLatency = 0;          // declared latency the processor must reproduce exactly
    bool valid() const noexcept { return sampleRate > 0 && maxBlock > 0 && channels > 0; }
};

// Builds the CircuitConfig for a compiled graph (non-audio). Fails when the graph was compiled for
// a different sample rate or oversampling factor than the target (validated per configuration).
bool circuitConfigFromGraph(const blueprint::PreparedGraph& g, const GraphTarget& t, CircuitConfig& out, std::string& err);

class GraphProcessor {
public:
    bool prepare(const blueprint::PreparedGraph& g, const GraphTarget& t, std::string& err);  // non-audio
    std::uint64_t generation() const noexcept { return generation_; }
    CircuitRuntime& circuit() noexcept { return circuit_; }
    const CircuitRuntime& circuit() const noexcept { return circuit_; }
    const GraphTarget& target() const noexcept { return target_; }
private:
    CircuitRuntime circuit_;
    GraphTarget target_{};
    std::uint64_t generation_ = 0;
};

class GraphInstaller {
public:
    static constexpr std::uint32_t kSlots = 2;
    enum class Status : std::uint8_t { Published, Pending, Idle, Stale, Failed };
    struct Stats {
        std::uint64_t published = 0, coalesced = 0, stale = 0, failed = 0, retired = 0, lastPublished = 0;
        int live = 0;                // graph processors currently allocated (<= kSlots)
    };

    explicit GraphInstaller(const GraphTarget& target);
    GraphInstaller(const GraphInstaller&) = delete;
    GraphInstaller& operator=(const GraphInstaller&) = delete;

    // ---- worker thread ----
    Status submit(blueprint::PreparedHandle graph, std::string& error);
    Status service(std::string& error);     // retire released processors, publish the pending graph
    bool hasPending() const noexcept { return pending_ != nullptr; }
    Stats stats() const noexcept { return stats_; }
    const GraphTarget& target() const noexcept { return target_; }

    // ---- any thread ----
    std::uint64_t acknowledged() const noexcept { return pool_.acknowledgedGeneration(); }
    std::uint64_t releasedBelow() const noexcept { return releasedBelow_.load(std::memory_order_acquire); }

private:
    friend class Engine;
    struct Holder { std::unique_ptr<GraphProcessor> proc; };
    void retire() noexcept;

    GraphTarget target_;
    state::GenerationPool<Holder, kSlots> pool_;
    alignas(64) std::atomic<std::uint64_t> releasedBelow_{0};   // audio writes, worker reads
    blueprint::PreparedHandle pending_;                         // worker-private
    std::uint64_t lastSubmitted_ = 0;                           // worker-private
    Stats stats_{};                                             // worker-private
};

}  // namespace cd::runtime
