// CircuitDrift tests: UX-09 integrated realtime stress (cd::runtime::Engine and SlotEngine) under
// RtTrapScope. Zero allocations, deallocations, mutex locks, condition waits and OS I/O calls are
// allowed on the audio thread (lock/I-O counting needs Linux/glibc; elsewhere it is reported as
// "not instrumented" and only allocations are enforced).
// Duration: CD_STRESS_SECONDS of AUDIO time (default 5; nightly CI uses 3600 = the one-hour UX-09
// duration requirement; wall time and realtime factor are reported separately). Synthetic fixtures
// only; no physical validation. Random partitions {0..8192}, TransactionQueue events from a real
// producer thread, NaN/Inf/huge bursts, live graph installs (GraphInstaller worker) and prepared-
// engine swaps (tryInstallPrepared with off-audio reclamation).
// Owner: rtqual agent (test-only).
#include <fcntl.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#if defined(__unix__) || defined(__APPLE__)
#include <pthread.h>
#include <unistd.h>
#endif
#include "blueprint/Compiler.h"
#include "runtime/Engine.h"
#include "runtime/ProcessorGraph.h"
#include "runtime/SlotEngine.h"
#include "state/TransactionQueue.h"
#include "support/RtTrap.h"

namespace {
using namespace cd;
using namespace cd::runtime;
using cd::state::TransactionEvent;
using cd::state::TransactionQueue;
using cd::test::RtTrapCounts;
using cd::test::RtTrapScope;
using Clock = std::chrono::steady_clock;
int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

struct Lcg {
    uint64_t s;
    uint32_t u() { s = s * 6364136223846793005ull + 1442695040888963407ull; return uint32_t(s >> 33); }
    double d() { return double(u()) / 2147483648.0 - 1.0; }
    double unit() { return double(u()) / 2147483648.0; }
};

double stressSeconds() {
    const char* v = std::getenv("CD_STRESS_SECONDS");
    if (!v || !*v) return 5.0;
    char* end = nullptr;
    const double s = std::strtod(v, &end);
    if (end == v || !std::isfinite(s) || s <= 0 || s > 1e6) { std::printf("bad CD_STRESS_SECONDS '%s', using 5\n", v); return 5.0; }
    return s;
}

// ---------------- trap positive controls (explicit operator new/delete: clang elides plain new) ----
void* volatile g_sink = nullptr;
template <class Fn> RtTrapCounts underTrap(Fn&& fn) { RtTrapScope s; fn(); return s.counts(); }

void positiveControls() {
    auto a = underTrap([] { g_sink = ::operator new(32); ::operator delete(g_sink); });
    CHECK(a.allocations == 1 && a.deallocations == 1, "alloc control %ld/%ld", a.allocations, a.deallocations);
    if (!cd::test::kRtTrapLockIoInstrumented) {
        std::printf("UX-09 trap: lock/I-O NOT INSTRUMENTED on this platform (allocation trap only)\n");
        return;
    }
    std::mutex m;
    a = underTrap([&] { m.lock(); m.unlock(); });
    CHECK(a.mutexLocks == 1, "std::mutex lock not counted (%ld)", a.mutexLocks);
    a = underTrap([&] { std::lock_guard<std::mutex> g(m); });
    CHECK(a.mutexLocks == 1, "lock_guard not counted (%ld)", a.mutexLocks);
    a = underTrap([&] { if (m.try_lock()) m.unlock(); });
    CHECK(a.mutexLocks == 1, "try_lock not counted (%ld)", a.mutexLocks);
    pthread_mutex_t pm = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t pc = PTHREAD_COND_INITIALIZER;
    a = underTrap([&] {
        pthread_mutex_lock(&pm);
        timespec ts{}; clock_gettime(CLOCK_REALTIME, &ts);   // already elapsed: returns ETIMEDOUT at once
        pthread_cond_timedwait(&pc, &pm, &ts);
        pthread_mutex_unlock(&pm);
    });
    CHECK(a.mutexLocks == 1 && a.condWaits == 1, "pthread lock/cond_timedwait %ld/%ld", a.mutexLocks, a.condWaits);
    char path[] = "/tmp/cd_rttrap_XXXXXX";
    const int fd = mkstemp(path);   // outside scope: setup
    CHECK(fd >= 0, "mkstemp");
    if (fd >= 0) {
        char buf[8] = "abc";
        a = underTrap([&] { (void)!::write(fd, buf, 3); });
        CHECK(a.ioCalls == 1, "write() not counted (%ld)", a.ioCalls);
        a = underTrap([&] { (void)!::read(fd, buf, 0); });
        CHECK(a.ioCalls == 1, "read() not counted (%ld)", a.ioCalls);
        a = underTrap([&] { const int f2 = ::open(path, O_RDONLY); if (f2 >= 0) ::close(f2); });
        CHECK(a.ioCalls == 1, "open() not counted (%ld)", a.ioCalls);
        a = underTrap([&] { if (FILE* f = std::fopen(path, "ab")) { std::fwrite(buf, 1, 3, f); std::fclose(f); } });
        CHECK(a.ioCalls == 2, "fopen+fwrite not counted (%ld)", a.ioCalls);
        a = underTrap([&] { if (FILE* f = std::fopen(path, "ab")) { std::fprintf(f, "x%d", 1); std::fclose(f); } });
        CHECK(a.ioCalls == 2, "fopen+fprintf not counted (%ld)", a.ioCalls);
        ::close(fd);
        std::remove(path);
    }
    // Inert outside a scope, and worker threads are excluded from the audio thread's scope.
    std::atomic<int> go{0}, done{0};
    long workerLocks = -1;
    std::thread w([&] {
        while (!go.load()) std::this_thread::yield();
        for (int k = 0; k < 50; ++k) { m.lock(); m.unlock(); }
        { RtTrapScope ws; m.lock(); m.unlock(); workerLocks = ws.counts().mutexLocks; }
        done = 1;
    });
    a = underTrap([&] { go = 1; while (!done.load()) std::this_thread::yield(); });
    w.join();
    CHECK(a.violations() == 0 && workerLocks == 1, "worker leaked into scope (%ld) / worker own scope %ld", a.violations(), workerLocks);
    std::printf("UX-09 trap positive controls: alloc, std::mutex/lock_guard/try_lock, pthread lock + cond_timedwait, "
                "write/read/open/fopen/fwrite/fprintf counted; worker excluded\n");
}

// ---------------- stress ----------------
const char* kRcBlueprint = R"({"schemaVersion":1,"id":"stress.rc","name":"Synthetic RC fixture","modelRegistryVersion":"fixture-v1",
 "nodes":["0","in","out"],"components":[{"id":"R1","modelId":"fixture.resistor","pins":{"p":"in","n":"out"},"parameters":{"resistanceOhm":2200}},
 {"id":"C1","modelId":"fixture.capacitor","pins":{"p":"out","n":"0"},"parameters":{"capacitanceF":1e-8}}],
 "ports":{"input":"in","output":"out","ground":"0"},"macroBindings":[]})";

EngineConfig makeConfig(QualityMode mode) {
    EngineConfig c;
    c.sampleRate = 48000; c.maxBlock = 512; c.channels = 2;
    c.quality.mode = mode;
    c.circuit = syntheticDiodeClipper();
    c.initial.autoGain = true;
    return c;
}

// Producer thread: pushes random valid (and some invalid) events as a host/message thread would.
class Producer {
public:
    explicit Producer(TransactionQueue& q) : q_(q), th_([this] { run(); }) {}
    ~Producer() { stop(); }
    void stop() { if (th_.joinable()) { stop_ = true; th_.join(); } }
    uint64_t pushed() const { return pushed_.load(); }
private:
    void run() {
        Lcg g{777};
        uint64_t gen = 0;
        while (!stop_.load()) {
            for (int k = 0; k < 8; ++k) {
                const auto& p = core::kParams[g.u() % core::kParamCount];
                const uint32_t off = g.u() % 9000;   // beyond frames is clamped by the queue
                double v = g.unit();
                const uint32_t r = g.u() % 64;
                if (r == 0) v = std::numeric_limits<double>::quiet_NaN();
                else if (r == 1) v = 7.5;
                TransactionEvent e = TransactionEvent::param(p.hostId, v, off);
                if (r == 2) e = TransactionEvent::config(++gen, g.u() % 256, off);
                else if (r == 3) e = TransactionEvent::noteOff(uint8_t(g.u() % 16), uint8_t(g.u() % 128), off);
                else if (r == 4) e = TransactionEvent::allNotesOff(state::kOmniChannel, off);
                q_.push(e);
                ++pushed_;
            }
            q_.flushPending();
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }
    TransactionQueue& q_;
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> pushed_{0};
    std::thread th_;
};

// Worker thread (non-audio): compiles/publishes graphs and builds prepared engines; reclaims retired ones.
struct Worker {
    GraphInstaller* inst = nullptr;
    Engine* builder = nullptr;                       // separate Engine used only for buildPrepared
    std::atomic<PreparedEngine*> mailbox{nullptr};   // worker -> audio
    std::atomic<PreparedEngine*> retired[4] = {};    // audio -> worker
    std::atomic<bool> stop{false};
    std::atomic<int> published{0}, built{0};
    std::thread th;
    std::string err;
    void start() {
        th = std::thread([this] {
            uint64_t gen = 0;
            int tick = 0;
            while (!stop.load()) {
                for (auto& r : retired) delete r.exchange(nullptr);
                if (inst) {
                    inst->service(err);
                    const auto rc = blueprint::compileText(kRcBlueprint, blueprint::ModelRegistry::syntheticFixtures(),
                                                           blueprint::RuntimeConfig{48000, 4, 1.0}, ++gen);
                    if (rc.ok && inst->submit(rc.graph, err) == GraphInstaller::Status::Published) ++published;
                }
                if (builder && (++tick % 3) == 0 && !mailbox.load()) {
                    auto h = builder->buildPrepared(makeConfig((tick / 3) % 2 ? QualityMode::Eco : QualityMode::Studio), err);
                    if (h) { mailbox.store(h.release()); ++built; }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
            }
            for (auto& r : retired) delete r.exchange(nullptr);
            delete mailbox.exchange(nullptr);
        });
    }
    void join() { stop = true; if (th.joinable()) th.join(); }
};

struct Totals {
    RtTrapCounts trap{};
    int64_t frames = 0, calls = 0, nonFinite = 0;
    double peak = 0, audioWall = 0;   // audioWall: wall seconds spent inside process calls
    int64_t installs = 0, stale = 0;
};

struct Signal {
    std::vector<double> l, r, ol, orr;
    Lcg g{42};
    Signal() : l(8192), r(8192), ol(8192), orr(8192) {}
    // Fills n frames; injects NaN/Inf/huge bursts occasionally.
    void fill(uint32_t n, int64_t pos) {
        for (uint32_t i = 0; i < n; ++i) {
            const double t = double(pos + i) / 48000.0;
            l[i] = 0.4 * std::sin(2 * M_PI * 110 * t) + 0.1 * g.d();
            r[i] = 0.3 * std::sin(2 * M_PI * 173 * t) + 0.1 * g.d();
        }
        if (n > 0 && g.u() % 150 == 0) {
            const uint32_t at = g.u() % n, len = 1 + g.u() % std::min<uint32_t>(n - at, 64);
            const uint32_t kind = g.u() % 4;
            for (uint32_t i = at; i < at + len; ++i) {
                const double bad = kind == 0 ? std::numeric_limits<double>::quiet_NaN() : kind == 1 ? std::numeric_limits<double>::infinity()
                                 : kind == 2 ? -std::numeric_limits<double>::infinity() : 1e12 * g.d();
                l[i] = bad;
                if (g.u() & 1) r[i] = bad;
            }
        }
    }
    uint32_t partition(int64_t remaining) {
        uint32_t n = g.u() % 1025;
        const uint32_t k = g.u() % 64;
        if (k == 0) n = 8192; else if (k == 1) n = 0; else if (k == 2) n = 1 + g.u() % 8192;
        if (int64_t(n) > remaining) n = uint32_t(remaining);
        return n;
    }
};

template <class Proc>
void runLoop(Proc& proc, TransactionQueue& queue, Worker& worker, Engine* swapTarget, int64_t totalFrames, Totals& t) {
    Signal sig;
    int64_t done = 0;
    std::unique_ptr<PreparedEngine> held;   // retired/rejected engine waiting for a free reclaim slot
    RtTrapScope scope;
    while (done < totalFrames) {
        const uint32_t n = sig.partition(totalFrames - done);
        sig.fill(n, done);   // test harness work is inside the scope too: it must not allocate/lock either
        const double* in[2] = {sig.l.data(), sig.r.data()};
        double* out[2] = {sig.ol.data(), sig.orr.data()};
        if (swapTarget) {
            if (held) {
                for (auto& slot : worker.retired) {
                    PreparedEngine* expected = nullptr;
                    if (slot.compare_exchange_strong(expected, held.get())) { held.release(); break; }
                }
            }
            if (!held) {
                if (PreparedEngine* p = worker.mailbox.exchange(nullptr)) {
                    std::unique_ptr<PreparedEngine> h(p);
                    const bool ok = swapTarget->tryInstallPrepared(h);
                    ++(ok ? t.installs : t.stale);
                    held = std::move(h);   // move into an EMPTY unique_ptr: nothing is destroyed here
                }
            }
        }
        const auto c0 = Clock::now();
        proc(ProcessContext{48000, done, 0, n, false}, AudioBuses{2, in, out}, queue);
        t.audioWall += std::chrono::duration<double>(Clock::now() - c0).count();
        for (uint32_t i = 0; i < n; ++i) {
            const double a = sig.ol[i], b = sig.orr[i];
            if (!std::isfinite(a) || !std::isfinite(b)) { ++t.nonFinite; continue; }
            t.peak = std::max(t.peak, std::max(std::fabs(a), std::fabs(b)));
        }
        done += n; t.frames += n; ++t.calls;
    }
    t.trap = scope.counts();
}

void report(const char* name, const Totals& t, double wall, double audioSec) {
    std::printf("UX-09 stress [%s]: %.1f s audio in %lld calls, wall %.1f s (realtime factor %.1fx; in-process %.1f s => %.1fx), "
                "allocs %ld frees %ld locks %ld condwaits %ld io %ld, peak %.4f, nonfinite %lld, prepared installs %lld stale %lld%s\n",
                name, audioSec, (long long)t.calls, wall, audioSec / wall, t.audioWall, audioSec / t.audioWall, t.trap.allocations,
                t.trap.deallocations, t.trap.mutexLocks, t.trap.condWaits, t.trap.ioCalls, t.peak, (long long)t.nonFinite,
                (long long)t.installs, (long long)t.stale, cd::test::kRtTrapLockIoInstrumented ? "" : " [lock/io NOT INSTRUMENTED]");
    CHECK(t.trap.violations() == 0, "%s: realtime violations alloc %ld free %ld lock %ld cond %ld io %ld", name, t.trap.allocations,
          t.trap.deallocations, t.trap.mutexLocks, t.trap.condWaits, t.trap.ioCalls);
    CHECK(t.nonFinite == 0 && t.peak <= 8.0 + 1e-9, "%s: non-finite %lld / peak %.4f beyond containment", name, (long long)t.nonFinite, t.peak);
}

void stressEngine(double seconds) {
    std::string e;
    Engine eng, builder;
    CHECK(eng.prepare(makeConfig(QualityMode::Studio), e), "prepare: %s", e.c_str());
    GraphInstaller inst(eng.graphTarget());
    CHECK(eng.attachGraphInstaller(&inst), "attach installer");
    TransactionQueue queue;
    Worker w; w.inst = &inst; w.builder = &builder;
    const auto total = int64_t(seconds * 48000);
    Totals t;
    const auto t0 = Clock::now();
    {
        Producer prod(queue);
        w.start();
        auto proc = [&](const ProcessContext& c, const AudioBuses& b, TransactionQueue& q) { eng.processQueued(c, b, q); };
        runLoop(proc, queue, w, &eng, total, t);
        prod.stop();
        std::printf("  producer pushed %llu events\n", (unsigned long long)prod.pushed());
    }
    w.join();
    const double wall = std::chrono::duration<double>(Clock::now() - t0).count();
    const auto d = eng.diagnostics();
    report("Engine", t, wall, double(total) / 48000);
    std::printf("  graphs published %d installed %lld rejected %lld; prepared built %d; param events %lld unsupported %lld unknown %lld "
                "sanitized inputs %lld, queue rejected %llu coalesced %llu\n",
                w.published.load(), (long long)d.graphInstalls, (long long)d.graphRejected, w.built.load(), (long long)d.paramEvents,
                (long long)d.unsupportedEvents, (long long)d.unknownEvents, (long long)d.inputNonFinite,
                (unsigned long long)queue.diagnostics().rejected, (unsigned long long)queue.diagnostics().sliceCoalesced);
    CHECK(d.inputNonFinite > 0, "NaN/Inf bursts never reached the engine");
    CHECK(d.paramEvents > 0, "no transaction events applied");
    CHECK(seconds < 2 || d.graphInstalls > 0, "no graph install happened (published %d)", w.published.load());
    CHECK(seconds < 2 || t.installs > 0, "no prepared-engine swap happened");
}

void stressSlotEngine(double seconds) {
    std::string e;
    SlotEngine se;
    CHECK(se.prepare(makeConfig(QualityMode::Studio), makeConfig(QualityMode::Eco), e), "SlotEngine prepare: %s", e.c_str());
    TransactionQueue queue;
    Worker w;   // unused for the A/B path (no installer/builder)
    const auto total = int64_t(seconds * 48000);
    Totals t;
    const auto t0 = Clock::now();
    {
        Producer prod(queue);
        Lcg g{9};
        int64_t calls = 0;
        auto proc = [&](const ProcessContext& c, const AudioBuses& b, TransactionQueue& q) {
            se.processQueued(c, b, q);
            if ((++calls % 97) == 0) se.mixer().setPosition(g.unit());   // A/B crossfade command
        };
        runLoop(proc, queue, w, nullptr, total, t);
        prod.stop();
    }
    const double wall = std::chrono::duration<double>(Clock::now() - t0).count();
    report("SlotEngine A/B", t, wall, double(total) / 48000);
}

}  // namespace

int main() {
    positiveControls();
    const double s = stressSeconds();
    std::printf("CD_STRESS_SECONDS = %.1f (audio time; default 5, nightly 3600)\n", s);
    stressEngine(s * 0.75);
    stressSlotEngine(s * 0.25);
    std::printf(g_fail ? "FAILED (%d)\n" : "ALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
