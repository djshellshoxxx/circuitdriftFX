// T17 telemetry contract tests (P09). SYNTHETIC producer only: no runtime engine is involved, and
// nothing here validates physical component behaviour (G01 gates remain open).
// Covers: value/unit/generation fidelity, gap marking on drop-on-full, stale flag, reader
// close/stall never blocking the writer, zero writer-side allocations (RtTrap with positive
// control), concurrent writer/reader, audio determinism independent of the reader.
#include "analysis/Telemetry.h"
#include "support/RtTrap.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <thread>

namespace {
using namespace cd::analysis;
int failures = 0;
#define CHECK(c) do { if (!(c)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

TelemetryFrame makeFrame(std::int64_t sampleIndex, std::uint32_t gen, std::uint32_t epoch = 0) {
    TelemetryFrame f;
    f.graphGeneration = gen;
    f.epoch = epoch;
    f.sampleIndex = sampleIndex;
    f.validityFlags = kValidRail | kValidXy | kValidSolver;
    f.railVolts = 92.25f;
    f.railNominalVolts = 120.0f;
    f.railFraction = 92.25f / 120.0f;
    f.solverIterations = 3;
    f.solverResidual = 1e-9f;
    f.componentCount = 3;
    for (std::uint32_t i = 0; i < 3; ++i) {
        f.components[i].componentId = 100 + i;
        f.components[i].flags = 1;
        f.components[i].temperatureK = 300.15f + 25.0f * float(i);  // 27, 52, 77 C
        f.components[i].dissipationW = 0.125f * float(i + 1);
        f.components[i].currentA = 1e-3f * float(i + 1);
        f.components[i].voltageV = 12.5f - float(i);
    }
    f.xyCount = 2;
    f.xyKind = std::uint32_t(XyKind::InputOutputVolts);
    f.xy[0] = {0.25f, 0.5f};
    f.xy[1] = {-0.25f, -0.5f};
    return f;
}

void fidelityAndUnits() {
    TelemetryChannel ch(16);
    TelemetryWriter w(ch);
    TelemetryReader r(ch);
    CHECK(w.publish(makeFrame(4800, 7, 2)));
    DisplayFrame d;
    CHECK(r.poll(d, 0.0));
    CHECK(d.raw.schemaVersion == kTelemetrySchemaVersion);
    CHECK(d.raw.graphGeneration == 7 && d.raw.epoch == 2 && d.raw.sampleIndex == 4800);
    CHECK(d.components[0].componentId == 100);
    CHECK(std::fabs(d.components[0].temperatureC - 27.0) < 1e-3);  // K -> C within float tolerance
    CHECK(std::fabs(d.components[2].temperatureC - 77.0) < 1e-3);
    CHECK(std::fabs(d.components[1].dissipationW - 0.25) < 1e-6);
    CHECK(std::fabs(d.components[2].currentA - 3e-3) < 1e-9);
    CHECK(std::fabs(d.components[1].voltageV - 11.5) < 1e-6);
    CHECK(std::fabs(d.raw.railVolts - 92.25) < 1e-6 && std::fabs(d.raw.railFraction - 0.76875) < 1e-6);
    CHECK(d.raw.xyCount == 2 && d.raw.xy[0].y == 0.5f && d.raw.xyKind == std::uint32_t(XyKind::InputOutputVolts));
    CHECK(d.missedBefore == 0 && !d.generationChanged && !d.epochChanged);
    CHECK(std::fabs(kelvinToCelsius(273.15)) < 1e-12);
    CHECK(!r.poll(d, 0.0));  // empty
}

void generationsAndGaps() {
    TelemetryChannel ch(8);
    TelemetryWriter w(ch);
    TelemetryReader r(ch);
    // Overload: reader never drains; 8 accepted, the rest dropped (new frame dropped, counter).
    int accepted = 0;
    for (int i = 0; i < 20; ++i) accepted += w.publish(makeFrame(i * 100, 1)) ? 1 : 0;
    CHECK(accepted == 8);
    CHECK(w.droppedFull() == 12 && w.published() == 8);
    DisplayFrame d;
    int n = 0;
    while (r.poll(d, 0.0)) {
        CHECK(d.raw.sampleIndex == n * 100);  // oldest retained, no overwrite
        CHECK(d.missedBefore == 0);
        ++n;
    }
    CHECK(n == 8);
    // Next frame after the drops carries the gap (seq 20 vs expected 8).
    CHECK(w.publish(makeFrame(9999, 2)));
    CHECK(r.poll(d, 0.1));
    CHECK(d.missedBefore == 12 && r.totalMissed() == 12);
    CHECK(d.generationChanged && d.raw.graphGeneration == 2 && !d.epochChanged);
    CHECK(w.publish(makeFrame(10000, 2, 5)));
    CHECK(r.poll(d, 0.1));
    CHECK(d.epochChanged && !d.generationChanged && d.missedBefore == 0);
    // pollLatest: drain to the newest, accumulating flags.
    w.publish(makeFrame(1, 2, 5));
    w.publish(makeFrame(2, 3, 5));
    w.publish(makeFrame(3, 3, 5));
    CHECK(r.pollLatest(d, 0.2));
    CHECK(d.raw.sampleIndex == 3 && d.generationChanged && d.missedBefore == 0);
    CHECK(!r.pollLatest(d, 0.2));
}

void staleFlag() {
    TelemetryChannel ch(8);
    TelemetryWriter w(ch);
    TelemetryReader r(ch);
    CHECK(r.isStale(0.0));  // nothing ever received
    w.publish(makeFrame(0, 1));
    DisplayFrame d;
    r.poll(d, 1.0);
    CHECK(!r.isStale(1.0) && !r.isStale(1.25));
    CHECK(r.isStale(1.2501));
    CHECK(r.isStale(std::numeric_limits<double>::quiet_NaN()));
}

void writerNeverAllocatesNorBlocks() {
    TelemetryChannel ch(32);
    TelemetryWriter w(ch);
    TelemetryReader r(ch);
    const TelemetryFrame f = makeFrame(0, 1);
    {
        cd::test::RtTrapScope scope;
        for (int i = 0; i < 1000; ++i) w.publish(f);  // fills then drops
        CHECK(scope.counts().allocations == 0 && scope.counts().deallocations == 0);
    }
    {   // positive control: the trap sees an explicit allocation in this harness
        cd::test::RtTrapScope scope;
        void* volatile p = ::operator new(64);
        ::operator delete(p);
        CHECK(scope.counts().allocations >= 1);
    }
    CHECK(w.droppedFull() > 900);
    // Reader close: writer keeps returning immediately and counts discards.
    r.close();
    {
        cd::test::RtTrapScope scope;
        for (int i = 0; i < 1000; ++i) CHECK(!w.publish(f));
        CHECK(scope.counts().allocations == 0);
    }
    CHECK(w.discardedNoReader() == 1000);
}

// Stalled reader on another thread: writer finishes a large burst without waiting for it.
void stalledReaderDoesNotBlock() {
    TelemetryChannel ch(16);
    TelemetryWriter w(ch);
    TelemetryReader r(ch);  // never polled: a total stall
    std::atomic<bool> done{false};
    const auto t0 = std::chrono::steady_clock::now();
    std::thread prod([&] {
        const TelemetryFrame f = makeFrame(0, 1);
        for (int i = 0; i < 200000; ++i) w.publish(f);
        done.store(true);
    });
    prod.join();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(done.load());
    CHECK(ms < 5000.0);
    CHECK(w.published() + w.droppedFull() == 200000);
}

// Concurrent producer/consumer with sample-index-derived payload to prove no torn frames.
void concurrentIntegrity() {
    TelemetryChannel ch(64);
    TelemetryWriter w(ch);
    TelemetryReader r(ch);
    constexpr int kN = 100000;
    std::atomic<bool> prodDone{false};
    std::thread prod([&] {
        for (int i = 0; i < kN; ++i) {
            TelemetryFrame f = makeFrame(i, 1);
            f.railVolts = float(i % 1000);
            f.components[0].voltageV = float(i % 1000) + 0.5f;
            f.xy[1].x = float(i % 1000);
            w.publish(f);
            if ((i & 1023) == 0) std::this_thread::yield();
        }
        prodDone.store(true);
    });
    std::uint64_t got = 0;
    std::int64_t last = -1;
    bool torn = false, ordered = true;
    DisplayFrame d;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
        if (r.poll(d, 0.0)) {
            ++got;
            const float expect = float(d.raw.sampleIndex % 1000);
            if (d.raw.railVolts != expect || d.components[0].voltageV != expect + 0.5f || d.raw.xy[1].x != expect)
                torn = true;
            if (d.raw.sampleIndex <= last) ordered = false;
            last = d.raw.sampleIndex;
        } else if (prodDone.load()) {
            break;
        } else {
            std::this_thread::yield();
        }
    }
    prod.join();
    while (r.poll(d, 0.0)) { ++got; last = d.raw.sampleIndex; }
    CHECK(!torn && ordered);
    CHECK(got == w.published());
    CHECK(got + w.droppedFull() == kN);
    CHECK(r.totalMissed() <= w.droppedFull());  // trailing drops have no later frame to expose them
}

// The deterministic synthetic "audio" must not depend on the telemetry consumer.
double syntheticAudio(TelemetryWriter& w, int mode, TelemetryReader* r) {
    double acc = 0, state = 0.1;
    DisplayFrame d;
    for (int i = 0; i < 20000; ++i) {
        state = 0.999 * state + 1e-3 * std::sin(0.01 * i);
        acc += state * state;
        if (i % 100 == 0) {
            TelemetryFrame f = makeFrame(i, 1);
            f.railVolts = float(state);
            w.publish(f);
        }
        if (mode == 1 && r && i % 700 == 0) r->poll(d, 0.0);  // slow reader
        if (mode == 2 && r && i == 5000) r->close();          // GUI closes mid-run
    }
    return acc;
}
void audioIndependentOfReader() {
    double ref;
    {
        TelemetryChannel ch(4);
        TelemetryWriter w(ch);
        TelemetryReader r(ch);
        r.close();
        ref = syntheticAudio(w, 0, nullptr);
    }
    for (int mode = 0; mode < 3; ++mode) {
        TelemetryChannel ch(4);
        TelemetryWriter w(ch);
        TelemetryReader r(ch);
        CHECK(syntheticAudio(w, mode, &r) == ref);  // bit-identical
    }
}

void frameLayout() {
    CHECK(sizeof(TelemetryFrame) % 8 == 0);
    CHECK(kMaxTelemetryComponents >= 8);
    TelemetryChannel ch(16);
    TelemetryWriter w(ch);
    TelemetryReader r(ch);
    TelemetryFrame f = makeFrame(0, 1);
    f.componentCount = 9999;  // writer clamps to bounds
    f.xyCount = 9999;
    w.publish(f);
    DisplayFrame d;
    CHECK(r.poll(d, 0.0));
    CHECK(d.raw.componentCount == std::uint32_t(kMaxTelemetryComponents));
    CHECK(d.raw.xyCount == std::uint32_t(kMaxTelemetryXy));
}
}  // namespace

int main() {
    fidelityAndUnits();
    generationsAndGaps();
    staleFlag();
    writerNeverAllocatesNorBlocks();
    stalledReaderDoesNotBlock();
    concurrentIntegrity();
    audioIndependentOfReader();
    frameLayout();
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("analysis telemetry (T17): all checks passed\n");
    return 0;
}
