// Background validation only. This service never installs a graph or touches audio.
// A single pending request replaces its predecessor; only the latest generation
// can be polled. Compiler probes observe cancellation on each electrical sample.
#pragma once
#include "blueprint/Compiler.h"
#include <condition_variable>
#include <optional>
#include <thread>

namespace cd::blueprint {

struct ValidationReport {
    Generation generation = 0;
    bool ok = false;
    Diagnostics diagnostics;
    CostReport cost;
    RuntimeConfig config;
};

class ValidationWorker {
public:
    explicit ValidationWorker(ModelRegistry registry = ModelRegistry::syntheticFixtures());
    ~ValidationWorker();
    ValidationWorker(const ValidationWorker&) = delete;
    ValidationWorker& operator=(const ValidationWorker&) = delete;

    // Message/worker thread only. Rejects excessive text before copying it.
    Generation request(std::string_view text, const RuntimeConfig& config);
    // Discards queued and completed reports and prevents active work publishing.
    // Cancelling does not enqueue a replacement compilation or failure report.
    void cancel();
    bool poll(ValidationReport& report);

private:
    struct Request {
        Generation generation;
        std::string text;
        RuntimeConfig config;
        std::stop_token cancellation;
    };
    void run();
    ModelRegistry registry_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::optional<Request> pending_;
    std::optional<ValidationReport> completed_;
    std::stop_source cancellation_;
    Generation latest_ = 0;
    bool stopping_ = false;
    std::thread worker_;
};

}  // namespace cd::blueprint
