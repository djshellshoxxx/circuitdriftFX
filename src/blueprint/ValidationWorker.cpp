#include "blueprint/ValidationWorker.h"
#include <exception>
#include <utility>

namespace cd::blueprint {

ValidationWorker::ValidationWorker(ModelRegistry registry) : registry_(std::move(registry)) {}

ValidationWorker::~ValidationWorker() {
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
        cancellation_.request_stop();
        pending_.reset();
    }
    ready_.notify_one();
    if (worker_.joinable()) worker_.join();
}

Generation ValidationWorker::request(std::string_view text, const RuntimeConfig& config) {
    const std::lock_guard lock(mutex_);
    cancellation_.request_stop();
    cancellation_ = std::stop_source{};
    ++latest_;
    completed_.reset();
    pending_.reset();
    if (text.size() > kMaxBytes) {
        ValidationReport report;
        report.generation = latest_;
        report.config = config;
        report.diagnostics.add(DiagCode::TooLarge, 1, "", "", "blueprint exceeds the 1 MiB UTF-8 bound");
        completed_ = std::move(report);
    } else {
        pending_ = Request{latest_, std::string(text), config, cancellation_.get_token()};
        if (!worker_.joinable()) {
            try { worker_ = std::thread([this] { run(); }); }
            catch (const std::exception&) {
                pending_.reset();
                ValidationReport report;
                report.generation = latest_;
                report.config = config;
                report.diagnostics.add(DiagCode::Internal, 1, "", "", "could not start the validation worker");
                completed_ = std::move(report);
            }
        }
        ready_.notify_one();
    }
    return latest_;
}

bool ValidationWorker::poll(ValidationReport& report) {
    const std::lock_guard lock(mutex_);
    if (!completed_) return false;
    report = std::move(*completed_);
    completed_.reset();
    return true;
}

void ValidationWorker::run() {
    for (;;) {
        std::optional<Request> request;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
            if (stopping_) return;
            request = std::move(pending_);
            pending_.reset();
        }
        ValidationReport report;
        report.generation = request->generation;
        report.config = request->config;
        try {
            auto compiled = compileText(request->text, registry_, request->config,
                                        request->generation, request->cancellation);
            report.ok = compiled.ok;
            report.cost = compiled.cost;
            report.diagnostics = std::move(compiled.diagnostics);
            // The candidate is destroyed on this worker. Audio and UI threads
            // never acquire or dispose of its solver storage.
        } catch (const std::exception&) {
            report.diagnostics.add(DiagCode::Internal, 1, "", "", "validation could not complete");
        }
        {
            const std::lock_guard lock(mutex_);
            if (!stopping_ && request->generation == latest_) completed_ = std::move(report);
        }
    }
}

}  // namespace cd::blueprint
