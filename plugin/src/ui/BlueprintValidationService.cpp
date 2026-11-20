#include "ui/BlueprintValidationService.h"
#include <exception>
#include <utility>

namespace cd::plugin {
namespace {
BlueprintReport failure(const juce::String& reason) {
    BlueprintReport report;
    report.diagnostics.add(reason);
    return report;
}
juce::String utf8(const std::string& value) {
    return juce::String::fromUTF8(value.data(), static_cast<int>(value.size()));
}
BlueprintReport describe(const blueprint::CompileResult& result, const blueprint::RuntimeConfig& configuration) {
    BlueprintReport report;
    report.ok = result.ok;
    report.components = result.cost.components;
    report.nodes = result.cost.nodes;
    report.nonlinear = result.cost.nonlinear;
    report.unknowns = result.cost.unknowns;
    report.estimatedFlopsPerSample = result.cost.estimatedFlopsPerSample;
    report.estimatedBytes = result.cost.estimatedBytes;
    report.sampleRate = configuration.sampleRate;
    report.oversampling = configuration.oversampling;
    report.syntheticModels = true;
    for (const auto& diagnostic : result.diagnostics.items) {
        auto location = utf8(diagnostic.component);
        if (!diagnostic.pin.empty()) location += "." + utf8(diagnostic.pin);
        if (location.isNotEmpty()) location += ": ";
        report.diagnostics.add("Check " + juce::String(diagnostic.check) + " [" +
            blueprint::diagCodeName(diagnostic.code) + "] " + location + utf8(diagnostic.message));
    }
    if (result.diagnostics.truncated) report.diagnostics.add("Additional diagnostics were truncated.");
    return report;
}
}  // namespace

BlueprintValidationService::~BlueprintValidationService() {
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
        activeCancellation_.request_stop();
        pending_.reset();
    }
    ready_.notify_one();
    if (worker_.joinable()) worker_.join();
}

void BlueprintValidationService::request(std::string json, blueprint::RuntimeConfig configuration) {
    if (json.size() > blueprint::kMaxBytes) {
        reject("Blueprint exceeds the 1 MiB UTF-8 limit.");
        return;
    }
    {
        const std::lock_guard lock(mutex_);
        activeCancellation_.request_stop();
        report_.reset();
        pending_ = Request{std::move(json), configuration, {}};
        if (!worker_.joinable()) {
            try {
                worker_ = std::thread([this] { run(); });
            } catch (const std::exception&) {
                pending_.reset();
                report_ = failure("Could not start the blueprint validation worker.");
            }
        }
    }
    ready_.notify_one();
}

void BlueprintValidationService::reject(const juce::String& reason) {
    const std::lock_guard lock(mutex_);
    activeCancellation_.request_stop();
    pending_.reset();
    report_ = failure(reason);
}

void BlueprintValidationService::cancel() {
    const std::lock_guard lock(mutex_);
    activeCancellation_.request_stop();
    pending_.reset();
    report_.reset();
}

bool BlueprintValidationService::take(BlueprintReport& out) {
    const std::lock_guard lock(mutex_);
    if (!report_) return false;
    out = std::move(*report_);
    report_.reset();
    return true;
}

void BlueprintValidationService::run() {
    for (;;) {
        std::optional<Request> request;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
            if (stopping_) return;
            request = std::move(pending_);
            pending_.reset();
            activeCancellation_ = request->cancellation;
        }
        BlueprintReport report;
        try {
            const auto registry = blueprint::ModelRegistry::syntheticFixtures();
            const auto result = blueprint::compileText(request->json, registry, request->configuration,
                                                        0, request->cancellation.get_token());
            report = describe(result, request->configuration);
            // The immutable compiled graph is destroyed here on this worker.
        } catch (const std::exception& error) {
            report = failure("Blueprint validation failed: " + juce::String::fromUTF8(error.what()));
        } catch (...) {
            report = failure("Blueprint validation failed unexpectedly.");
        }
        {
            const std::lock_guard lock(mutex_);
            if (!stopping_ && !request->cancellation.stop_requested()) report_ = std::move(report);
        }
    }
}

}  // namespace cd::plugin
