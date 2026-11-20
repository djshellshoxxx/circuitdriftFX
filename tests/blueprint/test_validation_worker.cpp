// The live validation service uses synthetic fixtures; no physical-model claim.
#include "blueprint/ValidationWorker.h"
#include "../state/check.h"
#include <chrono>
#include <thread>

using namespace cd::blueprint;
namespace {
const char* rc = R"({"schemaVersion":1,"id":"worker.rc","name":"Worker RC fixture","modelRegistryVersion":"fixture-v1",
  "nodes":["0","in","out"],"components":[
    {"id":"R1","modelId":"fixture.resistor","pins":{"p":"in","n":"out"},"parameters":{"resistanceOhm":10000}},
    {"id":"C1","modelId":"fixture.capacitor","pins":{"p":"out","n":"0"},"parameters":{"capacitanceF":1e-8}}],
  "ports":{"input":"in","output":"out","ground":"0"},"macroBindings":[]})";

bool waitFor(ValidationWorker& worker, ValidationReport& result) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (worker.poll(result)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}
}

int main() {
    const RuntimeConfig config{8000, 1, 1.0};
    ValidationWorker worker;
    ValidationReport result;
    CHECK(!worker.poll(result));
    const auto first = worker.request(rc, config);
    CHECK(waitFor(worker, result));
    CHECK(result.generation == first && result.ok && result.diagnostics.empty());
    CHECK(result.cost.components == 2 && result.cost.nodes == 3 && result.cost.unknowns == 3);
    CHECK(result.cost.estimatedFlopsPerSample > 0 && result.cost.validatedForThisConfigurationOnly);
    CHECK(result.config.sampleRate == 8000 && result.config.oversampling == 1);
    CHECK(!worker.poll(result));
    // Only the newest text/configuration can produce a visible report.
    for (int i = 0; i < 40; ++i) worker.request(i % 2 == 0 ? rc : "{}", config);
    const auto last = worker.request(rc, config);
    CHECK(waitFor(worker, result));
    CHECK(result.generation == last && result.ok);
    const auto bad = worker.request("{}", config);
    CHECK(waitFor(worker, result));
    CHECK(result.generation == bad && !result.ok && !result.diagnostics.empty());
    worker.request(rc, {384000, 16, 1.0});
    const std::string excess(kMaxBytes + 1, ' ');
    const auto oversized = worker.request(excess, config);
    CHECK(worker.poll(result));
    CHECK(result.generation == oversized && !result.ok && result.diagnostics.has(DiagCode::TooLarge));
    CHECK(!worker.poll(result));
    const auto unsupported = worker.request(rc, {0, 1, 1.0});
    CHECK(waitFor(worker, result));
    CHECK(result.generation == unsupported && !result.ok && result.diagnostics.has(DiagCode::BadConfig));
    std::stop_source cancel;
    cancel.request_stop();
    const auto cancelled = compileText(rc, ModelRegistry::syntheticFixtures(), config, 9, cancel.get_token());
    CHECK(!cancelled.ok && !cancelled.graph && cancelled.diagnostics.has(DiagCode::Cancelled));
    {
        ValidationWorker closing;
        closing.request(rc, {384000, 16, 1.0});
    }  // teardown cancels queued/active work and joins before members are freed
    return TEST_RESULT("blueprint validation worker");
}
