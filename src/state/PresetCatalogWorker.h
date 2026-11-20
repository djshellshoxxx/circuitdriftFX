// Local preset discovery off the message/audio threads. One active scan and one
// replaceable pending request; only the latest generation can be published.
#pragma once
#include "state/PresetLibrary.h"
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace cd::state {

struct PresetCatalogResult {
    std::uint64_t generation = 0;
    PresetCatalog catalog;
};

class PresetCatalogWorker {
public:
    explicit PresetCatalogWorker(std::filesystem::path directory, PresetScanLimits limits = {});
    // An injected provider is useful for deterministic concurrency tests. It runs
    // only on the worker and must preserve the catalog bounds/cancellation contract.
    using Scanner = std::function<PresetCatalog(std::stop_token)>;
    explicit PresetCatalogWorker(Scanner scanner);
    ~PresetCatalogWorker();
    PresetCatalogWorker(const PresetCatalogWorker&) = delete;
    PresetCatalogWorker& operator=(const PresetCatalogWorker&) = delete;

    // Message/worker thread only. Construction is lazy: no thread or I/O until
    // the first request. A new request cancels and invalidates all older work.
    std::uint64_t request();
    bool poll(PresetCatalogResult& result);

private:
    struct Request {
        std::uint64_t generation;
        std::stop_token cancellation;
    };
    void run();
    Scanner scan_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::optional<Request> pending_;
    std::optional<PresetCatalogResult> completed_;
    std::stop_source cancellation_;
    std::uint64_t latest_ = 0;
    bool stopping_ = false;
    std::thread worker_;
};

}  // namespace cd::state
