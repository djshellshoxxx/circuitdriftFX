// CircuitDrift TEST-ONLY audio-thread trap harness (UX-09 instrumentation primitive).
//
// WHAT: a thread-local, RAII "callback scope". While a CallbackScope is alive on a thread, any
// of these calls made BY THAT THREAD are counted in a thread-local violation counter:
//   New       operator new / new[] (plain, nothrow, aligned)
//   Delete    operator delete / delete[] (plain, sized, nothrow, aligned) on a non-null pointer
//   Malloc    malloc, calloc, realloc, memalign, aligned_alloc, posix_memalign
//   Free      free on a non-null pointer
//   MutexLock pthread_mutex_lock, pthread_mutex_timedlock (so std::mutex::lock too).
//             pthread_mutex_trylock is NOT counted (non-blocking).
//   FileIo    open/open64/openat/openat64/creat, read/write/pread/pwrite, fopen/fopen64, fread,
//             fwrite, fputs, puts, printf, fprintf, vprintf, vfprintf and their _FORTIFY_SOURCE
//             forms (__printf_chk, __fprintf_chk, __read_chk, __open_2, ...), so iostream/ofstream
//             and printf-style logging are caught when they reach these dynamic symbols.
// Hooks only bump thread-local counters. They never log, lock or allocate, so they cannot
// perturb the code under test. Threads without a live scope are never counted, so worker
// threads running concurrently are excluded by construction.
//
// SCOPE LIMITS (read before relying on a green result):
//   * Platform: Linux + glibc only (symbol interposition from the test executable, glibc
//     __libc_malloc family, dlsym(RTLD_NEXT)). Windows and macOS are NOT qualified: this header
//     fails to compile there on purpose.
//   * NOT compatible with ASan/TSan/MSan: those runtimes own malloc/operator new. The trap
//     tests therefore build as a separate NON-sanitized executable (cd_rt_trap_tests); this
//     header #errors if a sanitizer is active.
//   * Link the rt_trap object only into test executables, never into cd_core/cd_dsp/cd_state.
//     Link it statically into the executable (thread-local state must be local-exec TLS).
//   * Not covered: rwlock/condvar/semaphores, futex or other raw syscalls, mmap/brk growth,
//     stdio calls not listed (putchar, fputc, fgets, fscanf...), statically linked libc, code
//     calling __libc_* directly. Free of nullptr / delete of nullptr is a standard no-op and
//     is not counted.
//   * An exception thrown inside a scope allocates (__cxa_allocate_exception) and is counted.
//
// NOT A QUALIFICATION: a zero count from primitive-level smoke tests (tests/test_rt_trap.cpp)
// does NOT satisfy UX-09's required one-hour integrated audio stress test. That test needs the
// integrated process entry point, host-like block partitioning, concurrent worker/UI activity
// and a one-hour run, none of which exist yet. Do not mark UX-09 done from this harness.
#pragma once

#include <cstdint>  // defines __GLIBC__ on glibc; must precede the platform check

#if !defined(__linux__) || !defined(__GLIBC__)
#error "rt_trap is Linux/glibc only; Windows and macOS are NOT qualified (see rt_trap.h)."
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#error "rt_trap cannot be built with ASan/TSan; use the separate non-sanitized trap executable."
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#error "rt_trap cannot be built with ASan/TSan/MSan; use the separate non-sanitized trap executable."
#endif
#endif

namespace cd::rt_trap {

enum class Kind : unsigned { New, Delete, Malloc, Free, MutexLock, FileIo, Count };
constexpr unsigned kKindCount = static_cast<unsigned>(Kind::Count);

struct Counts {
    std::uint64_t byKind[kKindCount] = {};
    std::uint64_t of(Kind k) const noexcept { return byKind[static_cast<unsigned>(k)]; }
    std::uint64_t total() const noexcept {
        std::uint64_t t = 0;
        for (auto c : byKind) t += c;
        return t;
    }
};

// RAII scope for the calling thread. Nestable (inner exit keeps the outer scope active). Counts
// are monotonic per thread; a scope reports what happened since its own construction (an outer
// scope therefore includes its nested scopes). Not copyable or movable; must be destroyed on
// the thread that constructed it. Constructor/destructor are noexcept and allocation-free.
class CallbackScope {
public:
    CallbackScope() noexcept;
    ~CallbackScope();
    CallbackScope(const CallbackScope&) = delete;
    CallbackScope& operator=(const CallbackScope&) = delete;

    Counts counts() const noexcept;                       // since this scope was constructed
    std::uint64_t violations() const noexcept { return counts().total(); }

    static bool active() noexcept;                        // calling thread has a live scope
    static unsigned depth() noexcept;                     // nesting depth on calling thread

private:
    Counts baseline_;
};

}  // namespace cd::rt_trap
