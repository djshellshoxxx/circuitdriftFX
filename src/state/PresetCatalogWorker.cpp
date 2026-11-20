#include "state/PresetCatalogWorker.h"
#include <exception>
#include <utility>

namespace cd::state {

PresetCatalogWorker::PresetCatalogWorker(std::filesystem::path directory, PresetScanLimits limits)
    : PresetCatalogWorker([library = PresetLibrary(std::move(directory)), limits](std::stop_token cancellation) {
        return library.scan(limits, cancellation);
    }) {}

PresetCatalogWorker::PresetCatalogWorker(Scanner scanner) : scan_(std::move(scanner)) {}

PresetCatalogWorker::~PresetCatalogWorker() {
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
        cancellation_.request_stop();
        pending_.reset();
    }
    ready_.notify_one();
    if (worker_.joinable()) worker_.join();
}

std::uint64_t PresetCatalogWorker::request() {
    const std::lock_guard lock(mutex_);
    cancellation_.request_stop();
    cancellation_ = std::stop_source{};
    ++latest_;
    completed_.reset();
    pending_ = Request{latest_, cancellation_.get_token()};
    if (!worker_.joinable()) {
        try { worker_ = std::thread([this] { run(); }); }
        catch (const std::exception&) {
            pending_.reset();
            PresetCatalogResult result;
            result.generation = latest_;
            result.catalog.error = "Could not start the preset catalog worker.";
            completed_ = std::move(result);
        }
    }
    ready_.notify_one();
    return latest_;
}

bool PresetCatalogWorker::poll(PresetCatalogResult& result) {
    const std::lock_guard lock(mutex_);
    if (!completed_) return false;
    result = std::move(*completed_);
    completed_.reset();
    return true;
}

void PresetCatalogWorker::run() {
    for (;;) {
        std::optional<Request> request;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
            if (stopping_) return;
            request = std::move(pending_);
            pending_.reset();
        }
        PresetCatalogResult result;
        result.generation = request->generation;
        try {
            result.catalog = scan_(request->cancellation);
        } catch (const std::exception&) {
            result.catalog.error = "The preset catalog scan could not complete.";
        }
        {
            const std::lock_guard lock(mutex_);
            if (!stopping_ && request->generation == latest_ && !request->cancellation.stop_requested())
                completed_ = std::move(result);
        }
    }
}

}  // namespace cd::state
