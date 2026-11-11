// CircuitDrift tests: thread-local realtime trap scope (UX-09 instrumentation, test-only).
// Owner: runtime agent. RtTrap.cpp replaces every global operator new/delete overload (plain,
// array, sized, nothrow, aligned) with a pure pass-through to malloc/posix_memalign/free. While an
// RtTrapScope is alive on a thread, allocations and deallocations made BY THAT THREAD are counted;
// other threads are never counted and nothing is logged. Outside any scope the replacement is
// inert. ASan intercepts the underlying malloc/free, so leak/overflow checks still apply.
// UX-09 completion (RtTrapWrap.cpp): on Linux/glibc test executables are linked with
// -Wl,--wrap=<sym> (see CMakeLists.txt, test targets only) so calls to pthread_mutex_lock/trylock/
// timedlock, pthread_rwlock_rd/wrlock, pthread_cond_wait/timedwait and OS I/O (open/openat/read/
// write/fopen/fwrite/fread/fputs/fputc/puts/printf/fprintf) made from OBJECTS LINKED INTO THE TEST
// EXECUTABLE (engine static libs, std::mutex::lock inlined from headers, tests) are counted on the
// calling thread while a scope is active. NOT caught: work done inside shared libraries (libstdc++
// condition_variable::wait/iostream internals, libc-internal calls) and raw syscall(); a mutex lock
// taken by std::unique_lock around a condition wait is still caught via the lock. On other
// platforms (macOS/Windows) lock/IO counting is compiled out: kRtTrapLockIoInstrumented == false and
// tests must skip those positive controls (allocation counting still works everywhere).
// A bounded test run is not the one-hour UX-09 stress qualification.
// Linked into every test executable via tests/support/*.cpp.
#pragma once
#include <cstddef>   // defines __GLIBC__ (feature test below)

namespace cd::test {

#if defined(__linux__) && defined(__GLIBC__)
inline constexpr bool kRtTrapLockIoInstrumented = true;
#else
inline constexpr bool kRtTrapLockIoInstrumented = false;   // 'not instrumented': counters stay 0
#endif

struct RtTrapCounts {
    long allocations = 0;
    long deallocations = 0;
    long mutexLocks = 0;     // pthread_mutex_lock/trylock/timedlock, rwlock rd/wr lock
    long condWaits = 0;      // pthread_cond_wait/timedwait
    long ioCalls = 0;        // open/openat/read/write/fopen/fwrite/fread/fputs/fputc/puts/printf/fprintf
    long violations() const noexcept { return allocations + deallocations + mutexLocks + condWaits + ioCalls; }
};

class RtTrapScope {
public:
    RtTrapScope() noexcept;
    ~RtTrapScope();
    RtTrapScope(const RtTrapScope&) = delete;
    RtTrapScope& operator=(const RtTrapScope&) = delete;
    // Counts recorded on this thread since this scope opened (includes nested scopes' activity).
    RtTrapCounts counts() const noexcept;
private:
    RtTrapCounts start_;
};

// Whether a scope is active on the calling thread.
bool rtTrapActive() noexcept;

namespace detail {   // used by RtTrapWrap.cpp only; no-ops outside a scope
void noteLock() noexcept;
void noteCondWait() noexcept;
void noteIo() noexcept;
}

}  // namespace cd::test
