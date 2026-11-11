// CircuitDrift tests: linker-wrap shims backing the lock/I-O part of RtTrapScope (UX-09, test-only).
// Active only on Linux/glibc test executables linked with the -Wl,--wrap=... list in CMakeLists.txt.
// Each __wrap_X bumps a thread-local counter (only while a scope is open on the calling thread) and
// forwards to __real_X, so behaviour is unchanged and ASan's interceptors still see the real call.
// Elsewhere this file compiles to nothing (see kRtTrapLockIoInstrumented in RtTrap.h).
// KEEP IN SYNC with the wrap list in CMakeLists.txt.
#include "support/RtTrap.h"

#if defined(__linux__) && defined(__GLIBC__)
#include <fcntl.h>
#include <pthread.h>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <sys/types.h>
#include <unistd.h>

using cd::test::detail::noteCondWait;
using cd::test::detail::noteIo;
using cd::test::detail::noteLock;

extern "C" {
int __real_pthread_mutex_lock(pthread_mutex_t*);
int __real_pthread_mutex_trylock(pthread_mutex_t*);
int __real_pthread_mutex_timedlock(pthread_mutex_t*, const struct timespec*);
int __real_pthread_rwlock_rdlock(pthread_rwlock_t*);
int __real_pthread_rwlock_wrlock(pthread_rwlock_t*);
int __real_pthread_cond_wait(pthread_cond_t*, pthread_mutex_t*);
int __real_pthread_cond_timedwait(pthread_cond_t*, pthread_mutex_t*, const struct timespec*);
int __real_open(const char*, int, ...);
int __real_open64(const char*, int, ...);
int __real_openat(int, const char*, int, ...);
ssize_t __real_read(int, void*, size_t);
ssize_t __real_write(int, const void*, size_t);
FILE* __real_fopen(const char*, const char*);
FILE* __real_fopen64(const char*, const char*);
size_t __real_fwrite(const void*, size_t, size_t, FILE*);
size_t __real_fread(void*, size_t, size_t, FILE*);
int __real_fputs(const char*, FILE*);
int __real_fputc(int, FILE*);
int __real_puts(const char*);
extern int __vfprintf_chk(FILE*, int, const char*, va_list);

int __wrap_pthread_mutex_lock(pthread_mutex_t* m) { noteLock(); return __real_pthread_mutex_lock(m); }
int __wrap_pthread_mutex_trylock(pthread_mutex_t* m) { noteLock(); return __real_pthread_mutex_trylock(m); }
int __wrap_pthread_mutex_timedlock(pthread_mutex_t* m, const struct timespec* t) { noteLock(); return __real_pthread_mutex_timedlock(m, t); }
int __wrap_pthread_rwlock_rdlock(pthread_rwlock_t* l) { noteLock(); return __real_pthread_rwlock_rdlock(l); }
int __wrap_pthread_rwlock_wrlock(pthread_rwlock_t* l) { noteLock(); return __real_pthread_rwlock_wrlock(l); }
int __wrap_pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m) { noteCondWait(); return __real_pthread_cond_wait(c, m); }
int __wrap_pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m, const struct timespec* t) {
    noteCondWait();
    return __real_pthread_cond_timedwait(c, m, t);
}

int __wrap_open(const char* p, int flags, ...) {
    noteIo();
    mode_t mode = 0;
    if ((flags & O_CREAT) || (flags & __O_TMPFILE) == __O_TMPFILE) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
    return __real_open(p, flags, mode);
}
int __wrap_open64(const char* p, int flags, ...) {
    noteIo();
    mode_t mode = 0;
    if ((flags & O_CREAT) || (flags & __O_TMPFILE) == __O_TMPFILE) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
    return __real_open64(p, flags, mode);
}
int __wrap_openat(int dirfd, const char* p, int flags, ...) {
    noteIo();
    mode_t mode = 0;
    if ((flags & O_CREAT) || (flags & __O_TMPFILE) == __O_TMPFILE) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
    return __real_openat(dirfd, p, flags, mode);
}
ssize_t __wrap_read(int fd, void* b, size_t n) { noteIo(); return __real_read(fd, b, n); }
ssize_t __wrap_write(int fd, const void* b, size_t n) { noteIo(); return __real_write(fd, b, n); }
FILE* __wrap_fopen(const char* p, const char* m) { noteIo(); return __real_fopen(p, m); }
FILE* __wrap_fopen64(const char* p, const char* m) { noteIo(); return __real_fopen64(p, m); }
size_t __wrap_fwrite(const void* b, size_t s, size_t n, FILE* f) { noteIo(); return __real_fwrite(b, s, n, f); }
size_t __wrap_fread(void* b, size_t s, size_t n, FILE* f) { noteIo(); return __real_fread(b, s, n, f); }
int __wrap_fputs(const char* s, FILE* f) { noteIo(); return __real_fputs(s, f); }
int __wrap_fputc(int c, FILE* f) { noteIo(); return __real_fputc(c, f); }
int __wrap_puts(const char* s) { noteIo(); return __real_puts(s); }
// printf family: counted, then formatted with the (unwrapped) v* functions. Fortified _chk variants
// are what distro compilers emit at -O2 with _FORTIFY_SOURCE.
int __wrap_printf(const char* fmt, ...) {
    noteIo();
    va_list ap; va_start(ap, fmt); const int r = std::vprintf(fmt, ap); va_end(ap); return r;
}
int __wrap_fprintf(FILE* f, const char* fmt, ...) {
    noteIo();
    va_list ap; va_start(ap, fmt); const int r = std::vfprintf(f, fmt, ap); va_end(ap); return r;
}
int __wrap___printf_chk(int flag, const char* fmt, ...) {
    noteIo();
    va_list ap; va_start(ap, fmt); const int r = __vfprintf_chk(stdout, flag, fmt, ap); va_end(ap); return r;
}
int __wrap___fprintf_chk(FILE* f, int flag, const char* fmt, ...) {
    noteIo();
    va_list ap; va_start(ap, fmt); const int r = __vfprintf_chk(f, flag, fmt, ap); va_end(ap); return r;
}
}  // extern "C"
#endif
