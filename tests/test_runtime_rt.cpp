// CircuitDrift tests: UX-09 realtime trap (allocation/deallocation) at the Engine::process boundary.
// Positive controls prove the trap fires and excludes other threads. The bounded 60 s-of-audio
// stress run is NOT the one-hour UX-09 qualification, and mutex/OS-I/O traps are not implemented.
// Synthetic fixtures only. Owner: runtime agent.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "runtime/Engine.h"
#include "support/RtTrap.h"

namespace {
using namespace cd;
using namespace cd::runtime;
using cd::test::RtTrapCounts;
using cd::test::RtTrapScope;
int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

void* volatile g_sink = nullptr;  // defeats new/delete elision
struct alignas(64) Wide { double v[8]; };

template <class Fn> RtTrapCounts underTrap(Fn&& fn) {
    RtTrapScope scope;
    fn();
    return scope.counts();
}

void positive_controls() {
    auto a = underTrap([] { g_sink = new int(7); });
    delete static_cast<int*>(g_sink);
    CHECK(a.allocations == 1 && a.deallocations == 0, "plain new not trapped (%ld/%ld)", a.allocations, a.deallocations);
    a = underTrap([] { g_sink = new double[16]; delete[] static_cast<double*>(g_sink); });
    CHECK(a.allocations == 1 && a.deallocations == 1, "array new/delete not trapped");
    a = underTrap([] { g_sink = new Wide; delete static_cast<Wide*>(g_sink); });
    CHECK(a.allocations == 1 && a.deallocations == 1, "aligned new/delete not trapped");
    a = underTrap([] { g_sink = new (std::nothrow) int(3); delete static_cast<int*>(g_sink); });
    CHECK(a.allocations == 1 && a.deallocations == 1, "nothrow new not trapped");
    g_sink = new int(1);
    a = underTrap([] { delete static_cast<int*>(g_sink); });
    CHECK(a.allocations == 0 && a.deallocations == 1, "delete-only not trapped");
    a = underTrap([] { std::vector<double> v(100); g_sink = v.data(); });
    CHECK(a.allocations >= 1 && a.deallocations >= 1, "std::vector not trapped");
    // Nested scopes: outer sees inner activity; no counting outside any scope.
    {
        RtTrapScope outer;
        auto inner = underTrap([] { g_sink = new int(2); delete static_cast<int*>(g_sink); });
        CHECK(inner.allocations == 1 && outer.counts().allocations == 1, "nesting");
    }
    CHECK(!cd::test::rtTrapActive(), "scope still active");
    // Worker-thread allocations are excluded from the audio thread's scope.
    std::atomic<int> go{0}, done{0};
    std::atomic<long> workerCount{0};
    std::thread worker([&] {
        while (!go.load()) std::this_thread::yield();
        for (int k = 0; k < 100; ++k) { void* p = ::operator new(64); ::operator delete(p); }
        { RtTrapScope ws; void* p = ::operator new(64); ::operator delete(p); workerCount = ws.counts().allocations; }
        done = 1;
    });
    a = underTrap([&] {
        go = 1;
        while (!done.load()) std::this_thread::yield();
    });
    worker.join();
    CHECK(a.allocations == 0 && a.deallocations == 0 && workerCount == 1, "worker activity leaked into scope (%ld) or worker scope %ld",
          a.allocations, long(workerCount));
    std::printf("UX-09 trap positive controls: plain/array/aligned/nothrow/delete-only/vector/nesting trapped; worker excluded\n");
}

EngineConfig stressConfig(int maxNewtonOverride = 0) {
    EngineConfig c;
    c.sampleRate = 48000; c.maxBlock = 512; c.channels = 2;
    c.circuit = syntheticDiodeClipper();
    c.initial.autoGain = true;
    c.maxNewtonOverride = maxNewtonOverride;
    return c;
}

struct Lcg {
    uint64_t s;
    uint32_t u() { s = s * 6364136223846793005ull + 1442695040888963407ull; return uint32_t(s >> 33); }
    double d() { return double(u()) / 2147483648.0 - 1.0; }
};

// Engine::process at the boundary: every path under the trap must record zero activity.
void process_boundary() {
    std::string e;
    Engine eng;
    CHECK(eng.prepare(stressConfig(), e), "prepare %s", e.c_str());
    // Positive control at the same boundary: the identical harness flags preparation work.
    auto prepCount = underTrap([&] { Engine tmp; tmp.prepare(stressConfig(), e); });
    CHECK(prepCount.allocations > 0 && prepCount.deallocations > 0, "trap missed Engine::prepare allocations");
    auto handle = eng.buildPrepared(stressConfig(), e);
    std::vector<double> l(8192), r(8192), ol(8192), orr(8192);
    Lcg g{1};
    for (std::size_t n = 0; n < l.size(); ++n) { l[n] = 0.5 * g.d(); r[n] = 0.5 * std::sin(0.01 * double(n)); }
    const double* in[2] = {l.data(), r.data()};
    double* out[2] = {ol.data(), orr.data()};
    ParamEvent evs[4] = {{0, ParamTarget::Mix, 0.7}, {13, ParamTarget::HpfCutoffHz, 80}, {200, ParamTarget::HostBypass, 1},
                         {300, ParamTarget::HostBypass, 0}};
    auto c = underTrap([&] {
        for (uint32_t frames : {0u, 1u, 17u, 64u, 127u, 512u, 8192u, 0u, 333u}) {
            ProcessBlock b; b.ctx = {48000, 0, 0, frames, false}; b.buses = {2, in, out}; b.events = evs; b.eventCount = 4;
            eng.process(b);
        }
        eng.reset();
        CHECK(eng.tryInstallPrepared(handle), "install");     // swap only; old engine returned, not freed
        ProcessBlock b; b.ctx = {48000, 0, 1, 512, false}; b.buses = {2, in, out};
        eng.process(b);
        ProcessBlock bad; bad.ctx = {44100, 0, 1, 64, false}; bad.buses = {2, in, out};
        eng.process(bad);                                     // rate mismatch path
    });
    handle.reset();  // off-audio reclamation of the replaced engine
    CHECK(c.allocations == 0 && c.deallocations == 0, "Engine audio paths allocated %ld / freed %ld", c.allocations, c.deallocations);
    // Fault paths: containment (huge input) and latched solver fallback.
    auto fc = stressConfig(1);
    fc.initial.softClipEnabled = false;  // let the 1e5 input reach the +-8 containment
    Engine f; f.prepare(fc, e);
    for (std::size_t n = 0; n < l.size(); ++n) l[n] = 1e5 * std::sin(0.003 * double(n));
    c = underTrap([&] {
        for (int k = 0; k < 16; ++k) {
            ProcessBlock b; b.ctx = {48000, 0, 0, 512, false}; b.buses = {2, in, out};
            f.process(b);
        }
    });
    const auto d = f.diagnostics();
    CHECK(c.allocations == 0 && c.deallocations == 0 && d.circuit.fallbackLatched && d.output.fallbackLatched,
          "fault paths allocated %ld or did not trigger", c.allocations);
    std::printf("UX-09 process boundary: 0 allocations/deallocations across partitions, events, reset, install, mismatch, fault paths\n");
}

// Bounded stress: 60 s of stereo 48 kHz audio, random partitions/events/hostile bursts, under trap.
void stress_60s() {
    std::string e;
    Engine eng;
    CHECK(eng.prepare(stressConfig(), e), "prepare %s", e.c_str());
    const std::size_t total = 60 * 48000;
    std::vector<double> l(8192), r(8192), ol(8192), orr(8192);
    std::vector<ParamEvent> evs(8);
    Lcg g{42};
    double peak = 0;
    bool finite = true;
    std::size_t done = 0;
    long blocks = 0;
    const auto t0 = std::chrono::steady_clock::now();
    RtTrapCounts c;
    {
        RtTrapScope scope;
        while (done < total) {
            uint32_t n = g.u() % 1025;
            if (g.u() % 97 == 0) n = 8192;
            if (n > total - done) n = uint32_t(total - done);
            for (uint32_t i = 0; i < n; ++i) {
                const double t = double(done + i) / 48000.0;
                l[i] = 0.4 * std::sin(2 * M_PI * 110 * t) + 0.1 * g.d();
                r[i] = 0.3 * std::sin(2 * M_PI * 173 * t) + 0.1 * g.d();
            }
            if (n > 0 && g.u() % 211 == 0) l[g.u() % n] = std::numeric_limits<double>::quiet_NaN();
            uint32_t ne = g.u() % 4;
            for (uint32_t k = 0; k < ne; ++k) {
                ParamEvent& ev = evs[k];
                ev.offset = n ? g.u() % n : 0;
                ev.target = ParamTarget(g.u() % 7);  // excludes host bypass (separately tested)
                ev.value = ev.target == ParamTarget::HpfCutoffHz ? 10 + 240 * (0.5 + 0.5 * g.d())
                         : ev.target == ParamTarget::Mix ? 0.5 + 0.5 * g.d() : 6.0 * g.d();
            }
            const double* in[2] = {l.data(), r.data()};
            double* out[2] = {ol.data(), orr.data()};
            ProcessBlock b; b.ctx = {48000, int64_t(done), 0, n, false}; b.buses = {2, in, out};
            b.events = evs.data(); b.eventCount = ne;
            eng.process(b);
            for (uint32_t i = 0; i < n; ++i) {
                finite = finite && std::isfinite(ol[i]) && std::isfinite(orr[i]);
                peak = std::max(peak, std::max(std::fabs(ol[i]), std::fabs(orr[i])));
            }
            done += n;
            ++blocks;
        }
        c = scope.counts();
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const auto d = eng.diagnostics();
#if defined(__SANITIZE_ADDRESS__)
    const char* build = "ASan/UBSan build";
#else
    const char* build = "unsanitized build";
#endif
    std::printf("UX-09 bounded stress: 60 s stereo 48 kHz Studio clipper, %ld calls, wall %.1f s incl. harness (%s; not a CPU "
                "qualification), allocs %ld frees %ld, peak %.6f, failed substeps %ld, input sanitized %ld, thermal ticks %ld\n",
                blocks, wall, build, c.allocations, c.deallocations, peak, d.circuit.failedSubsteps, d.inputNonFinite, d.circuit.thermalTicks);
    CHECK(c.allocations == 0 && c.deallocations == 0, "stress allocations %ld frees %ld", c.allocations, c.deallocations);
    CHECK(finite && peak <= 8.0 && d.samples == int64_t(total) && d.circuit.thermalTicks == 60000, "stress output/clock");
}

}  // namespace

int main() {
    positive_controls();
    process_boundary();
    stress_60s();
    std::printf(g_fail ? "FAILED (%d)\n" : "ALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
