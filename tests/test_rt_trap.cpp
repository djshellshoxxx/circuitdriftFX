// Audio-thread trap harness tests (UX-09 instrumentation primitive). Linux/glibc, non-sanitized.
//
// Part 1: trap self-tests (positive controls, negative control, thread exclusion, nesting).
// Part 2: PRIMITIVE SMOKE TEST ONLY. Runs the existing cd_dsp process APIs inside a callback
//         scope and expects zero violations. This does NOT satisfy the UX-09 requirement of a
//         one-hour integrated audio stress test and must not be reported as doing so.
// No CHECK/printf is ever evaluated while a scope is alive (printf is itself trapped).
#include "rt_trap/rt_trap.h"

#include "dsp/DeviceManifest.h"
#include "dsp/Halfband2x.h"
#include "dsp/MnaSolver.h"
#include "dsp/Oversampling.h"
#include "dsp/OutputProtection.h"
#include "dsp/ThermalNetwork.h"

#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using cd::rt_trap::CallbackScope;
using cd::rt_trap::Counts;
using cd::rt_trap::Kind;

int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (false)

// Defeat the optimizer's right to delete allocation/free pairs.
template <class T> inline void escape(T* p) noexcept { asm volatile("" : : "g"(p) : "memory"); }

// Run stmt inside a fresh scope; evaluate CHECK only after the scope has closed.
#define TRAPPED(kind, ...) do { Counts c_; { CallbackScope s_; __VA_ARGS__; c_ = s_.counts(); } \
    if (c_.of(kind) == 0) { std::printf("FAIL line %d: trap did not fire: %s\n", __LINE__, #kind); ++failures; } } while (false)

struct alignas(64) Overaligned { char bytes[64]; };

// ---- Part 1: trap self-tests -----------------------------------------------------------

void positiveNewDelete() {
    TRAPPED(Kind::New, int* p = new int(1); escape(p); delete p);
    TRAPPED(Kind::Delete, int* p = new int(1); escape(p); delete p);
    TRAPPED(Kind::New, int* p = new int[4]; escape(p); delete[] p);
    TRAPPED(Kind::Delete, int* p = new int[4]; escape(p); delete[] p);
    TRAPPED(Kind::New, int* p = new (std::nothrow) int(1); escape(p); delete p);
    TRAPPED(Kind::New, auto* p = new Overaligned; escape(p); delete p);
    TRAPPED(Kind::Delete, auto* p = new Overaligned; escape(p); delete p);
    TRAPPED(Kind::New, std::vector<double> v; v.push_back(1.0); escape(v.data()));
    TRAPPED(Kind::New, std::string s(100, 'x'); escape(s.data()));
    // Exact counts for a balanced pair.
    Counts c;
    {
        CallbackScope s;
        int* p = new int(1);
        escape(p);
        delete p;
        c = s.counts();
    }
    CHECK(c.of(Kind::New) == 1 && c.of(Kind::Delete) == 1);
    CHECK(c.of(Kind::Malloc) == 0 && c.of(Kind::Free) == 0);  // operator new bypasses the malloc trap
}

void positiveMalloc() {
    void* (*volatile mallocFn)(std::size_t) = &std::malloc;
    void (*volatile freeFn)(void*) = &std::free;
    TRAPPED(Kind::Malloc, void* p = mallocFn(16); escape(p); freeFn(p));
    TRAPPED(Kind::Free, void* p = mallocFn(16); escape(p); freeFn(p));
    TRAPPED(Kind::Malloc, void* p = std::calloc(4, 4); escape(p); freeFn(p));
    TRAPPED(Kind::Malloc, void* p = mallocFn(8); p = std::realloc(p, 64); escape(p); freeFn(p));
    TRAPPED(Kind::Malloc, void* p = nullptr; int rc = posix_memalign(&p, 64, 64); escape(&rc); escape(p); freeFn(p));
    TRAPPED(Kind::Malloc, void* p = std::aligned_alloc(64, 64); escape(p); freeFn(p));
    Counts c;
    {
        CallbackScope s;
        void* p = mallocFn(16);
        escape(p);
        freeFn(p);
        c = s.counts();
    }
    CHECK(c.of(Kind::Malloc) == 1 && c.of(Kind::Free) == 1);
    CHECK(c.of(Kind::New) == 0 && c.of(Kind::Delete) == 0);
}

void positiveMutex() {
    pthread_mutex_t pm = PTHREAD_MUTEX_INITIALIZER;
    TRAPPED(Kind::MutexLock, pthread_mutex_lock(&pm); pthread_mutex_unlock(&pm));
    std::mutex sm;
    TRAPPED(Kind::MutexLock, sm.lock(); sm.unlock());
    TRAPPED(Kind::MutexLock, std::lock_guard<std::mutex> g(sm); escape(&g));
    timespec far{};
    clock_gettime(CLOCK_REALTIME, &far);
    far.tv_sec += 60;
    TRAPPED(Kind::MutexLock, int rc = pthread_mutex_timedlock(&pm, &far); escape(&rc); pthread_mutex_unlock(&pm));
    // try_lock is non-blocking and deliberately not counted (documented in rt_trap.h).
    Counts c;
    {
        CallbackScope s;
        const int rc = pthread_mutex_trylock(&pm);
        escape(&rc);
        pthread_mutex_unlock(&pm);
        c = s.counts();
    }
    CHECK(c.of(Kind::MutexLock) == 0);
    // Exact count: one lock, one count; the lock really worked (mutex released afterwards).
    {
        CallbackScope s;
        pthread_mutex_lock(&pm);
        pthread_mutex_unlock(&pm);
        c = s.counts();
    }
    CHECK(c.of(Kind::MutexLock) == 1);
    CHECK(pthread_mutex_trylock(&pm) == 0);
    pthread_mutex_unlock(&pm);
}

void positiveIo() {
    const int zero = ::open("/dev/zero", O_RDWR);
    FILE* sink = std::fopen("/dev/null", "w");
    CHECK(zero >= 0 && sink != nullptr);
    char buf[8] = {};
    TRAPPED(Kind::FileIo, ssize_t n = ::read(zero, buf, sizeof buf); escape(&n));
    TRAPPED(Kind::FileIo, ssize_t n = ::write(zero, buf, sizeof buf); escape(&n));
    TRAPPED(Kind::FileIo, ssize_t n = ::pread(zero, buf, sizeof buf, 0); escape(&n));
    TRAPPED(Kind::FileIo, ssize_t n = ::pwrite(zero, buf, sizeof buf, 0); escape(&n));
    TRAPPED(Kind::FileIo, int fd = ::open("/dev/null", O_WRONLY); escape(&fd); if (fd >= 0) ::close(fd));
    TRAPPED(Kind::FileIo, int fd = ::openat(AT_FDCWD, "/dev/null", O_WRONLY); escape(&fd); if (fd >= 0) ::close(fd));
    TRAPPED(Kind::FileIo, FILE* f = std::fopen("/dev/null", "w"); escape(f); if (f) std::fclose(f));
    TRAPPED(Kind::FileIo, std::fprintf(sink, "violation %d\n", 1));
    TRAPPED(Kind::FileIo, std::fputs("violation\n", sink));
    TRAPPED(Kind::FileIo, std::fwrite(buf, 1, sizeof buf, sink));
    TRAPPED(Kind::FileIo, std::size_t n = std::fread(buf, 1, 1, sink); escape(&n));  // fails (write-only), still trapped
    TRAPPED(Kind::FileIo, std::ofstream o("/dev/null"); o << "violation" << std::endl);
    // Exact data path still works through the interposer.
    CHECK(::read(zero, buf, sizeof buf) == static_cast<ssize_t>(sizeof buf));
    {
        Counts c;
        {
            CallbackScope s;
            ssize_t n = ::read(zero, buf, sizeof buf);
            escape(&n);
            c = s.counts();
        }
        CHECK(c.of(Kind::FileIo) == 1);
    }
    if (sink) std::fclose(sink);
    if (zero >= 0) ::close(zero);
}

void negativeControl() {
    Counts c;
    {
        CallbackScope s;
        double acc = 0;
        double stack[64];
        for (int i = 0; i < 64; ++i) stack[i] = i * 0.5;
        for (int k = 0; k < 10000; ++k)
            for (int i = 0; i < 64; ++i) acc += std::sin(stack[i] + k);
        std::atomic<int> a{0};
        a.fetch_add(1);
        int* none = nullptr;
        delete none;                 // standard no-op: not counted
        std::free(nullptr);          // standard no-op: not counted
        escape(&acc);
        c = s.counts();
        if (!(acc == acc)) c.byKind[0] = 99;  // keep acc live; NaN would show as a failure below
    }
    CHECK(c.total() == 0);
}

void workerThreadsExcluded() {
    // A: unscoped worker allocates/locks/writes heavily while main holds a scope: main sees zero.
    {
        std::atomic<int> go{0}, done{0};
        const int devNull = ::open("/dev/null", O_WRONLY);
        std::thread worker([&] {
            while (!go.load()) std::this_thread::yield();
            std::mutex m;
            for (int i = 0; i < 20000; ++i) {
                int* p = new int(i);
                escape(p);
                delete p;
                void* q = std::malloc(32);
                escape(q);
                std::free(q);
                m.lock();
                m.unlock();
                const ssize_t n = ::write(devNull, "x", 1);
                escape(&n);
            }
            done.store(1);
        });
        Counts c;
        {
            CallbackScope s;
            go.store(1);
            while (!done.load()) std::this_thread::yield();
            c = s.counts();
        }
        worker.join();
        if (devNull >= 0) ::close(devNull);
        CHECK(c.total() == 0);
    }
    // B: both threads scoped concurrently; counters are strictly per thread and exact.
    {
        std::atomic<int> go{0}, workerDone{0};
        Counts wc;
        std::thread worker([&] {
            while (!go.load()) std::this_thread::yield();
            {
                CallbackScope s;
                for (int i = 0; i < 11; ++i) {
                    int* p = new int(i);
                    escape(p);
                    delete p;
                }
                wc = s.counts();
            }
            workerDone.store(1);
        });
        Counts mc;
        {
            CallbackScope s;
            go.store(1);
            for (int i = 0; i < 5; ++i) {
                int* p = new int(i);
                escape(p);
                delete p;
            }
            while (!workerDone.load()) std::this_thread::yield();
            mc = s.counts();
        }
        worker.join();
        CHECK(mc.of(Kind::New) == 5 && mc.of(Kind::Delete) == 5 && mc.total() == 10);
        CHECK(wc.of(Kind::New) == 11 && wc.of(Kind::Delete) == 11 && wc.total() == 22);
    }
    // C: a fresh thread does not inherit its parent's scope.
    {
        bool childActive = true;
        unsigned childDepth = 99;
        {
            CallbackScope parent;  // thread spawn/join inside the scope is the parent's own cost
            std::thread t([&] {
                childActive = CallbackScope::active();
                childDepth = CallbackScope::depth();
            });
            t.join();
        }
        CHECK(!childActive && childDepth == 0);
    }
}

void nestingAndExitSafety() {
    CHECK(!CallbackScope::active() && CallbackScope::depth() == 0);
    Counts outerC, innerC;
    bool outerOk = false;
    {
        CallbackScope outer;
        bool a1 = CallbackScope::active();
        unsigned d1 = CallbackScope::depth(), d3 = 0;
        {
            CallbackScope inner;
            int* p = new int(1);
            escape(p);
            delete p;
            innerC = inner.counts();
            d3 = CallbackScope::depth();
        }
        bool a2 = CallbackScope::active();  // inner exit must keep outer active
        unsigned d2 = CallbackScope::depth();
        int* q = new int(2);
        escape(q);
        delete q;  // still counted by outer
        outerC = outer.counts();
        outerOk = a1 && d1 == 1 && a2 && d2 == 1 && d3 == 2;
    }
    CHECK(outerOk);
    CHECK(innerC.of(Kind::New) == 1 && innerC.of(Kind::Delete) == 1);
    CHECK(outerC.of(Kind::New) == 2 && outerC.of(Kind::Delete) == 2);
    CHECK(!CallbackScope::active() && CallbackScope::depth() == 0);

    // Allocation after the outermost exit is not counted; a new scope starts from zero.
    int* p = new int(3);
    escape(p);
    delete p;
    {
        CallbackScope fresh;
        CHECK(fresh.violations() == 0);
    }

    // Exception unwinding closes the scope (the exception allocation itself is a violation).
    Counts excC;
    bool caught = false;
    unsigned excDepth = 0;
    {
        CallbackScope outer;
        try {
            CallbackScope inner;
            throw 42;
        } catch (int) {
            caught = true;
        }
        excC = outer.counts();
        excDepth = CallbackScope::depth();
    }
    CHECK(excDepth == 1);
    CHECK(caught && excC.total() > 0);  // __cxa_allocate_exception hit malloc: counted, as documented
    CHECK(!CallbackScope::active() && CallbackScope::depth() == 0);

    // Scope on a thread that exits while the scope is destroyed normally; no state leaks to others.
    Counts tc;
    std::thread t([&] {
        {
            CallbackScope s;
            int* x = new int(4);
            escape(x);
            delete x;
            tc = s.counts();
        }
    });
    t.join();
    CHECK(tc.total() == 2 && !CallbackScope::active());
}

// ---- Part 2: PRIMITIVE SMOKE TEST ONLY (not the one-hour integrated stress test) --------

using namespace cd::dsp;

static_assert(noexcept(std::declval<ThermalNetwork&>().step(nullptr)));
static_assert(noexcept(std::declval<GainMatcher&>().update(0.0, 0.0)));
static_assert(noexcept(std::declval<ButterworthHpf&>().process(0.0)));
static_assert(noexcept(std::declval<Halfband2x&>().downsample(0.0, 0.0)));
static_assert(noexcept(std::declval<Oversampler&>().up(nullptr, nullptr, 0)));
static_assert(noexcept(std::declval<Oversampler&>().down(nullptr, nullptr, 0)));
static_assert(noexcept(std::declval<MnaSolver&>().step()));

NetElement source(int node, double value) {
    NetElement e;
    e.kind = NetElement::Kind::VoltageSource;
    e.a = node;
    e.value = value;
    return e;
}

NetElement device(const char* id, int a, int b, double p0, double p1 = 0, double p2 = 0) {
    NetElement e;
    e.model = findSyntheticFixture(id);
    e.a = a;
    e.b = b;
    e.param[0] = p0;
    e.param[1] = p1;
    e.param[2] = p2;
    return e;
}

void smokeThermalAndProtection() {
    ThermalNetwork::Config cfg;
    cfg.n = 2;
    cfg.capacity[0] = 2;
    cfg.capacity[1] = 1;
    cfg.toAmbient[0] = .2;
    cfg.toAmbient[1] = .3;
    cfg.coupling[0][1] = cfg.coupling[1][0] = .5;
    ThermalNetwork net;
    CHECK(net.prepare(cfg, 1e-3, 298.15));
    ButterworthHpf hpf;
    CHECK(hpf.design(48000, 20));
    GainMatcher gm;
    gm.prepare(48000);
    Halfband2x hb;
    hb.prepare();

    double acc = 0;
    Counts c;
    {
        CallbackScope s;
        for (int i = 0; i < 20000; ++i) {
            const double power[2] = {1.0 + 0.5 * std::sin(i * 1e-3), 0.25};
            net.step(power);
            const double x = 0.8 * std::sin(i * 0.05);
            const double y = softClip(hpf.process(3 * x));
            acc += y + gm.update(x, y) + gm.updateMeanSquares(x * x, y * y);
            double y0, y1;
            hb.upsample(x, y0, y1);
            acc += hb.downsample(y0, y1);
        }
        hpf.reset();
        hb.reset();
        c = s.counts();
    }
    CHECK(c.total() == 0);
    CHECK(std::isfinite(acc) && std::isfinite(net.temperature(0)) && net.temperature(0) > 298.15);
}

void smokeOversampler() {
    constexpr int kBlock = 256;
    for (OsFamily fam : {OsFamily::Fir, OsFamily::Iir}) {
        for (int factor : {1, 2, 4, 8, 16}) {
            Oversampler os;
            CHECK(os.prepare({factor, fam}, kBlock));
            std::vector<double> in(kBlock), hi(kBlock * 16), out(kBlock);
            double acc = 0;
            Counts c;
            {
                CallbackScope s;
                for (int blk = 0; blk < 40; ++blk) {
                    const int n = (blk % 7 == 0) ? 0 : (blk * 37) % kBlock + 1;  // includes zero frames
                    for (int i = 0; i < n; ++i) in[i] = 0.7 * std::sin((blk * kBlock + i) * 0.11);
                    os.up(in.data(), hi.data(), n);
                    for (int i = 0; i < n * factor; ++i) hi[i] = softClip(2.0 * hi[i]);
                    os.down(hi.data(), out.data(), n);
                    for (int i = 0; i < n; ++i) acc += out[i];
                }
                os.reset();
                c = s.counts();
            }
            CHECK(c.total() == 0);
            CHECK(std::isfinite(acc));
        }
    }
}

void smokeMnaSolver() {
    Netlist n;
    n.nodes = 2;
    n.elements = {source(1, 0), device("fixture.resistor", 1, 2, 1000),
                  device("fixture.diode", 2, 0, 1e-14, 1, 0.025852),
                  device("fixture.capacitor", 2, 0, 1e-8)};
    MnaSolver solver;
    std::string error;
    CHECK(solver.prepare(n, 1.0 / 192000, {}, error));
    int ok = 0;
    double acc = 0;
    std::vector<double> state(solver.stateSize());
    Counts c;
    {
        CallbackScope s;
        for (int i = 0; i < 20000; ++i) {
            solver.setSource(0, 0.9 * std::sin(i * 0.02));
            ok += solver.step() ? 1 : 0;
            acc += solver.voltage(2);
            if ((i & 1023) == 0) solver.getState(state.data());
        }
        c = s.counts();
    }
    CHECK(c.total() == 0);
    CHECK(ok == 20000 && std::isfinite(acc) && solver.failures() == 0);

    // Teeth check: prepare paths DO allocate and the trap sees them, so the zero above is meaningful.
    MnaSolver other;
    {
        CallbackScope s;
        const bool prepared = other.prepare(n, 1.0 / 192000, {}, error);
        c = s.counts();
        escape(&prepared);
    }
    CHECK(c.of(Kind::New) > 0);
    Oversampler os;
    {
        CallbackScope s;
        const bool prepared = os.prepare({4, OsFamily::Fir}, 64);
        c = s.counts();
        escape(&prepared);
    }
    CHECK(c.of(Kind::New) > 0);
}
}  // namespace

int main() {
    positiveNewDelete();
    positiveMalloc();
    positiveMutex();
    positiveIo();
    negativeControl();
    workerThreadsExcluded();
    nestingAndExitSafety();
    smokeThermalAndProtection();
    smokeOversampler();
    smokeMnaSolver();
    std::printf("rt_trap: trap self-tests + PRIMITIVE SMOKE TEST ONLY (does NOT satisfy the UX-09 "
                "one-hour integrated stress test; Linux/glibc, non-sanitized)\n");
    if (failures == 0) std::printf("rt_trap: all passed\n");
    return failures == 0 ? 0 : 1;
}
