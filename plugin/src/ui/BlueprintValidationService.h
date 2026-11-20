// Message-thread requests and reports; numerical compilation stays on one worker.
// At most one active and one pending 1 MiB document. New requests cancel old work.
// Owns no processor/audio state and never installs the compiled graph.
#pragma once
#include "ui/EditorBridge.h"
#include "blueprint/Compiler.h"
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

namespace cd::plugin {
class BlueprintValidationService final {
public:
    BlueprintValidationService() = default;
    ~BlueprintValidationService();
    BlueprintValidationService(const BlueprintValidationService&) = delete;
    BlueprintValidationService& operator=(const BlueprintValidationService&) = delete;
    void request(std::string json, blueprint::RuntimeConfig configuration);
    void reject(const juce::String& reason);
    void cancel();
    // Delivered once. No ready report leaves out unchanged.
    bool take(BlueprintReport& out);
private:
    struct Request {
        std::string json;
        blueprint::RuntimeConfig configuration;
        std::stop_source cancellation;
    };
    void run();
    std::mutex mutex_;
    std::condition_variable ready_;
    std::optional<Request> pending_;
    std::optional<BlueprintReport> report_;
    std::stop_source activeCancellation_;
    bool stopping_ = false;
    std::thread worker_;
};
}  // namespace cd::plugin
