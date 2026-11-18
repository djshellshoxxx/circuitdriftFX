// TEST-ONLY audio-thread trap implementation. Linux/glibc, non-sanitized, static link into a test
// executable only. See rt_trap.h for scope, limits and the "not the one-hour stress test" notice.
//
// Mechanism: this object defines the global replaceable operator new/delete and the listed libc
// entry points in the executable, so the dynamic linker binds every caller (including libstdc++)
// to these definitions. Each hook (1) bumps a constant-initialised thread-local counter when the
// calling thread has a live CallbackScope, then (2) forwards to the real implementation:
// glibc's __libc_* allocator entry points, raw syscalls for the descriptor-level I/O, and
// dlsym(RTLD_NEXT) for mutex/stdio. Hooks never log, lock or allocate.
#include "rt_trap.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>

extern "C" {
void* __libc_malloc(std::size_t);
void* __libc_calloc(std::size_t, std::size_t);
void* __libc_realloc(void*, std::size_t);
void* __libc_memalign(std::size_t, std::size_t);
void __libc_free(void*);
}

namespace cd::rt_trap {
namespace {

struct State {
    unsigned depth = 0;
    std::uint64_t counts[kKindCount] = {};
};
// Constant-initialised, trivially destructible: no TLS wrapper, no lazy init, no allocation.
thread_local State tls;

inline void hit(Kind k) noexcept {
    State& s = tls;
    if (s.depth != 0) ++s.counts[static_cast<unsigned>(k)];
}

// Real implementations reached through dlsym(RTLD_NEXT); resolved before main, lazily as fallback
// for calls made earlier (static initialisers of other translation units).
struct Real {
    int (*mutexLock)(pthread_mutex_t*) = nullptr;
    int (*mutexTimedLock)(pthread_mutex_t*, const timespec*) = nullptr;
    FILE* (*fopen)(const char*, const char*) = nullptr;
    FILE* (*fopen64)(const char*, const char*) = nullptr;
    std::size_t (*fread)(void*, std::size_t, std::size_t, FILE*) = nullptr;
    std::size_t (*fwrite)(const void*, std::size_t, std::size_t, FILE*) = nullptr;
    int (*fputs)(const char*, FILE*) = nullptr;
    int (*puts)(const char*) = nullptr;
    int (*vfprintf)(FILE*, const char*, va_list) = nullptr;
    int (*vprintf)(const char*, va_list) = nullptr;
    int (*vfprintfChk)(FILE*, int, const char*, va_list) = nullptr;
    int (*vprintfChk)(int, const char*, va_list) = nullptr;
    std::size_t (*freadChk)(void*, std::size_t, std::size_t, std::size_t, FILE*) = nullptr;
    bool resolved = false;
};
Real real;

template <class F> void bind(F& f, const char* name) noexcept {
    f = reinterpret_cast<F>(dlsym(RTLD_NEXT, name));
}

void resolve() noexcept {
    if (real.resolved) return;
    bind(real.mutexLock, "pthread_mutex_lock");
    bind(real.mutexTimedLock, "pthread_mutex_timedlock");
    bind(real.fopen, "fopen");
    bind(real.fopen64, "fopen64");
    bind(real.fread, "fread");
    bind(real.fwrite, "fwrite");
    bind(real.fputs, "fputs");
    bind(real.puts, "puts");
    bind(real.vfprintf, "vfprintf");
    bind(real.vprintf, "vprintf");
    bind(real.vfprintfChk, "__vfprintf_chk");
    bind(real.vprintfChk, "__vprintf_chk");
    bind(real.freadChk, "__fread_chk");
    real.resolved = true;
}

[[gnu::constructor(101)]] void resolveEarly() { resolve(); }

inline Real& R() noexcept {
    if (!real.resolved) resolve();
    return real;
}

void* allocOrThrow(std::size_t n, std::size_t align) {
    for (;;) {
        void* p = align > alignof(std::max_align_t) ? __libc_memalign(align, n ? n : 1)
                                                    : __libc_malloc(n ? n : 1);
        if (p) return p;
        std::new_handler h = std::get_new_handler();
        if (!h) throw std::bad_alloc();
        h();
    }
}

void* allocNoThrow(std::size_t n, std::size_t align) noexcept {
    try {
        return allocOrThrow(n, align);
    } catch (...) {
        return nullptr;
    }
}

inline void release(void* p) noexcept {
    if (p) {
        hit(Kind::Delete);
        __libc_free(p);
    }
}

}  // namespace

// ---- scope ----

CallbackScope::CallbackScope() noexcept {
    for (unsigned i = 0; i < kKindCount; ++i) baseline_.byKind[i] = tls.counts[i];
    ++tls.depth;
}
CallbackScope::~CallbackScope() {
    if (tls.depth) --tls.depth;
}
Counts CallbackScope::counts() const noexcept {
    Counts c;
    for (unsigned i = 0; i < kKindCount; ++i) c.byKind[i] = tls.counts[i] - baseline_.byKind[i];
    return c;
}
bool CallbackScope::active() noexcept { return tls.depth != 0; }
unsigned CallbackScope::depth() noexcept { return tls.depth; }

}  // namespace cd::rt_trap

using cd::rt_trap::Kind;
using cd::rt_trap::hit;
using cd::rt_trap::R;

// ---- operator new / delete (all replaceable forms) ----

void* operator new(std::size_t n) { hit(Kind::New); return cd::rt_trap::allocOrThrow(n, 0); }
void* operator new[](std::size_t n) { hit(Kind::New); return cd::rt_trap::allocOrThrow(n, 0); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { hit(Kind::New); return cd::rt_trap::allocNoThrow(n, 0); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { hit(Kind::New); return cd::rt_trap::allocNoThrow(n, 0); }
void* operator new(std::size_t n, std::align_val_t a) { hit(Kind::New); return cd::rt_trap::allocOrThrow(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { hit(Kind::New); return cd::rt_trap::allocOrThrow(n, static_cast<std::size_t>(a)); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { hit(Kind::New); return cd::rt_trap::allocNoThrow(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { hit(Kind::New); return cd::rt_trap::allocNoThrow(n, static_cast<std::size_t>(a)); }

void operator delete(void* p) noexcept { cd::rt_trap::release(p); }
void operator delete[](void* p) noexcept { cd::rt_trap::release(p); }
void operator delete(void* p, std::size_t) noexcept { cd::rt_trap::release(p); }
void operator delete[](void* p, std::size_t) noexcept { cd::rt_trap::release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { cd::rt_trap::release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { cd::rt_trap::release(p); }
void operator delete(void* p, std::align_val_t) noexcept { cd::rt_trap::release(p); }
void operator delete[](void* p, std::align_val_t) noexcept { cd::rt_trap::release(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { cd::rt_trap::release(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { cd::rt_trap::release(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { cd::rt_trap::release(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { cd::rt_trap::release(p); }

// ---- malloc family (glibc allocator underneath, so all pairs stay consistent) ----

extern "C" {

void* malloc(std::size_t n) noexcept { hit(Kind::Malloc); return __libc_malloc(n); }
void* calloc(std::size_t a, std::size_t b) noexcept { hit(Kind::Malloc); return __libc_calloc(a, b); }
void* realloc(void* p, std::size_t n) noexcept { hit(Kind::Malloc); return __libc_realloc(p, n); }
void* memalign(std::size_t a, std::size_t n) noexcept { hit(Kind::Malloc); return __libc_memalign(a, n); }
void* aligned_alloc(std::size_t a, std::size_t n) noexcept { hit(Kind::Malloc); return __libc_memalign(a, n); }
int posix_memalign(void** out, std::size_t a, std::size_t n) noexcept {
    hit(Kind::Malloc);
    if (a < sizeof(void*) || (a & (a - 1)) != 0) return EINVAL;
    void* p = __libc_memalign(a, n);
    if (!p) return ENOMEM;
    *out = p;
    return 0;
}
void free(void* p) noexcept {
    if (p) hit(Kind::Free);
    __libc_free(p);
}

// ---- mutex ----

int pthread_mutex_lock(pthread_mutex_t* m) noexcept {
    hit(Kind::MutexLock);
    return R().mutexLock(m);
}
int pthread_mutex_timedlock(pthread_mutex_t* m, const timespec* t) noexcept {
    hit(Kind::MutexLock);
    return R().mutexTimedLock(m, t);
}

// ---- descriptor-level I/O (raw syscalls: no dlsym, no recursion) ----

int open(const char* path, int flags, ...) {
    hit(Kind::FileIo);
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    return static_cast<int>(syscall(SYS_openat, AT_FDCWD, path, flags, mode));
}
int open64(const char* path, int flags, ...) {
    hit(Kind::FileIo);
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    return static_cast<int>(syscall(SYS_openat, AT_FDCWD, path, flags, mode));
}
int openat(int dirfd, const char* path, int flags, ...) {
    hit(Kind::FileIo);
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    return static_cast<int>(syscall(SYS_openat, dirfd, path, flags, mode));
}
int openat64(int dirfd, const char* path, int flags, ...) {
    hit(Kind::FileIo);
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    return static_cast<int>(syscall(SYS_openat, dirfd, path, flags, mode));
}
int creat(const char* path, mode_t mode) {
    hit(Kind::FileIo);
    return static_cast<int>(syscall(SYS_openat, AT_FDCWD, path, O_CREAT | O_WRONLY | O_TRUNC, mode));
}
ssize_t read(int fd, void* buf, std::size_t n) {
    hit(Kind::FileIo);
    return syscall(SYS_read, fd, buf, n);
}
ssize_t write(int fd, const void* buf, std::size_t n) {
    hit(Kind::FileIo);
    return syscall(SYS_write, fd, buf, n);
}
ssize_t pread(int fd, void* buf, std::size_t n, off_t off) {
    hit(Kind::FileIo);
    return syscall(SYS_pread64, fd, buf, n, off);
}
ssize_t pwrite(int fd, const void* buf, std::size_t n, off_t off) {
    hit(Kind::FileIo);
    return syscall(SYS_pwrite64, fd, buf, n, off);
}

// ---- stdio ----

FILE* fopen(const char* path, const char* mode) { hit(Kind::FileIo); return R().fopen(path, mode); }
FILE* fopen64(const char* path, const char* mode) { hit(Kind::FileIo); return R().fopen64(path, mode); }
std::size_t fread(void* p, std::size_t sz, std::size_t n, FILE* f) { hit(Kind::FileIo); return R().fread(p, sz, n, f); }
std::size_t fwrite(const void* p, std::size_t sz, std::size_t n, FILE* f) { hit(Kind::FileIo); return R().fwrite(p, sz, n, f); }
int fputs(const char* s, FILE* f) { hit(Kind::FileIo); return R().fputs(s, f); }
int puts(const char* s) { hit(Kind::FileIo); return R().puts(s); }
int vfprintf(FILE* f, const char* fmt, va_list ap) { hit(Kind::FileIo); return R().vfprintf(f, fmt, ap); }
int vprintf(const char* fmt, va_list ap) { hit(Kind::FileIo); return R().vprintf(fmt, ap); }
int fprintf(FILE* f, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}
int printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}

// ---- _FORTIFY_SOURCE entry points (Ubuntu/Debian GCC defaults to -D_FORTIFY_SOURCE=2 at -O1+,
// which rewrites printf/fprintf/open/read/fread to these). Same classification as the plain forms.
int __vfprintf_chk(FILE* f, int flag, const char* fmt, va_list ap) { hit(Kind::FileIo); return R().vfprintfChk(f, flag, fmt, ap); }
int __vprintf_chk(int flag, const char* fmt, va_list ap) { hit(Kind::FileIo); return R().vprintfChk(flag, fmt, ap); }
int __fprintf_chk(FILE* f, int flag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int n = __vfprintf_chk(f, flag, fmt, ap);
    va_end(ap);
    return n;
}
int __printf_chk(int flag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int n = __vprintf_chk(flag, fmt, ap);
    va_end(ap);
    return n;
}
std::size_t __fread_chk(void* p, std::size_t buflen, std::size_t sz, std::size_t n, FILE* f) {
    hit(Kind::FileIo);
    return R().freadChk(p, buflen, sz, n, f);
}
ssize_t __read_chk(int fd, void* buf, std::size_t n, std::size_t buflen) {
    if (n > buflen) abort();  // mirrors glibc's overflow check
    return read(fd, buf, n);
}
ssize_t __pread_chk(int fd, void* buf, std::size_t n, off_t off, std::size_t buflen) {
    if (n > buflen) abort();
    return pread(fd, buf, n, off);
}
int __open_2(const char* path, int flags) { return open(path, flags); }
int __open64_2(const char* path, int flags) { return open(path, flags); }
int __openat_2(int dirfd, const char* path, int flags) { return openat(dirfd, path, flags); }
int __openat64_2(int dirfd, const char* path, int flags) { return openat(dirfd, path, flags); }

}  // extern "C"
