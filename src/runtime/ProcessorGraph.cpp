// CircuitDrift runtime: graph processors and installer (see ProcessorGraph.h). Owner: cd_runtime.
#include "runtime/ProcessorGraph.h"
#include <cmath>
#include <utility>

namespace cd::runtime {

bool circuitConfigFromGraph(const blueprint::PreparedGraph& g, const GraphTarget& t, CircuitConfig& out, std::string& err) {
    if (!t.valid()) { err = "graph target not prepared"; return false; }
    if (g.config.sampleRate != t.sampleRate) { err = "graph compiled for a different sample rate"; return false; }
    if (g.config.oversampling != t.os.factor) { err = "graph compiled for a different oversampling factor"; return false; }
    if (g.inputElement < 0 || g.inputElement >= int(g.netlist.elements.size())) { err = "graph has no input drive source"; return false; }
    if (!(g.config.fullScaleVolts > 0 && std::isfinite(g.config.fullScaleVolts))) { err = "graph full-scale volts invalid"; return false; }
    CircuitConfig c;
    c.netlist = g.netlist;   // element model pointers reference g.models_; CircuitRuntime deep-copies them
    c.io.sourceElement = g.inputElement;
    c.io.voltsPerUnit = g.config.fullScaleVolts;
    c.io.outputNode = g.outputNode;
    c.io.unitsPerVolt = 1.0 / g.config.fullScaleVolts;
    c.sampleRate = t.sampleRate; c.maxBlock = t.maxBlock; c.channels = t.channels;
    c.os = t.os; c.maxNewton = t.maxNewton; c.padToFamilyMax = t.padToFamilyMax;
    out = std::move(c);
    return true;
}

bool GraphProcessor::prepare(const blueprint::PreparedGraph& g, const GraphTarget& t, std::string& err) {
    CircuitConfig c;
    if (!circuitConfigFromGraph(g, t, c, err)) return false;
    if (!circuit_.prepare(c, err)) return false;
    if (circuit_.latencySamples() != t.circuitLatency) {
        err = "graph processor latency differs from the active engine (needs host latency handshake)";
        return false;
    }
    target_ = t;
    generation_ = g.generation;
    return true;
}

GraphInstaller::GraphInstaller(const GraphTarget& target) : target_(target) {}

void GraphInstaller::retire() noexcept {
    (void)pool_.reclaim();
    const std::uint64_t below = releasedBelow_.load(std::memory_order_acquire);
    std::uint32_t taken[kSlots];
    std::uint32_t n = 0;
    for (std::uint32_t i; n < kSlots && (i = pool_.acquireFree()) != decltype(pool_)::kNone;) taken[n++] = i;
    for (std::uint32_t k = 0; k < n; ++k) {
        Holder* h = pool_.payload(taken[k]);
        if (h && h->proc && h->proc->generation() < below) {
            h->proc.reset();   // worker-thread destruction, after the audio thread released it
            ++stats_.retired;
            --stats_.live;
        }
        pool_.abandon(taken[k]);
    }
}

GraphInstaller::Status GraphInstaller::submit(blueprint::PreparedHandle graph, std::string& error) {
    if (!graph) { error = "null graph"; ++stats_.failed; return Status::Failed; }
    if (graph->generation == 0 || graph->generation > state::kMaxGeneration || graph->generation <= lastSubmitted_) {
        error = "stale or invalid graph generation";
        ++stats_.stale;
        return Status::Stale;
    }
    if (graph->config.sampleRate != target_.sampleRate || graph->config.oversampling != target_.os.factor) {
        error = "graph compiled for a different configuration";
        ++stats_.failed;
        return Status::Failed;
    }
    lastSubmitted_ = graph->generation;
    if (pending_) ++stats_.coalesced;   // newest Apply wins; no stacked fades
    pending_ = std::move(graph);
    return service(error);
}

GraphInstaller::Status GraphInstaller::service(std::string& error) {
    retire();
    if (!pending_) return Status::Idle;
    std::uint32_t taken[kSlots];
    std::uint32_t n = 0, use = decltype(pool_)::kNone;
    for (std::uint32_t i; n < kSlots && (i = pool_.acquireFree()) != decltype(pool_)::kNone;) {
        taken[n++] = i;
        if (use == decltype(pool_)::kNone && !pool_.payload(i)->proc) use = i;
    }
    auto abandonOthers = [&] { for (std::uint32_t k = 0; k < n; ++k) if (taken[k] != use) pool_.abandon(taken[k]); };
    if (use == decltype(pool_)::kNone) { abandonOthers(); return Status::Pending; }  // previous still fading
    auto proc = std::make_unique<GraphProcessor>();
    if (!proc->prepare(*pending_, target_, error)) {
        pool_.abandon(use);
        use = decltype(pool_)::kNone;
        abandonOthers();
        pending_.reset();
        ++stats_.failed;
        return Status::Failed;
    }
    const std::uint64_t gen = proc->generation();
    Holder* h = pool_.payload(use);
    h->proc = std::move(proc);
    ++stats_.live;
    if (!pool_.publish(use, gen)) {
        h->proc.reset();
        --stats_.live;
        pool_.abandon(use);
        use = decltype(pool_)::kNone;
        abandonOthers();
        pending_.reset();
        ++stats_.stale;
        error = "pool rejected generation";
        return Status::Stale;
    }
    abandonOthers();
    pending_.reset();
    ++stats_.published;
    stats_.lastPublished = gen;
    error.clear();
    return Status::Published;
}

}  // namespace cd::runtime
