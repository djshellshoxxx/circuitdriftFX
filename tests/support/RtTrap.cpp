// CircuitDrift tests: global operator new/delete replacement backing RtTrapScope (see RtTrap.h).
// Pure pass-through to malloc/posix_memalign/free; counting only on threads with an active scope.
#include "support/RtTrap.h"
#include <cstddef>
#include <cstdlib>
#include <new>

namespace {

thread_local int tDepth = 0;
thread_local long tAllocs = 0;
thread_local long tFrees = 0;
thread_local long tLocks = 0;
thread_local long tConds = 0;
thread_local long tIo = 0;

inline void noteAlloc() noexcept { if (tDepth > 0) ++tAllocs; }
inline void noteFree(void* p) noexcept { if (p && tDepth > 0) ++tFrees; }

void* allocOrNull(std::size_t n) noexcept { return std::malloc(n ? n : 1); }

void* allocAlignedOrNull(std::size_t n, std::size_t align) noexcept {
    if (align < sizeof(void*)) align = sizeof(void*);
    void* p = nullptr;
    if (posix_memalign(&p, align, n ? n : 1) != 0) return nullptr;
    return p;
}

void* allocOrThrow(std::size_t n) {
    for (;;) {
        if (void* p = allocOrNull(n)) { noteAlloc(); return p; }
        std::new_handler h = std::get_new_handler();
        if (!h) throw std::bad_alloc();
        h();
    }
}

void* allocAlignedOrThrow(std::size_t n, std::size_t align) {
    for (;;) {
        if (void* p = allocAlignedOrNull(n, align)) { noteAlloc(); return p; }
        std::new_handler h = std::get_new_handler();
        if (!h) throw std::bad_alloc();
        h();
    }
}

void* allocNothrow(std::size_t n) noexcept {
    try { return allocOrThrow(n); } catch (...) { return nullptr; }
}

void* allocAlignedNothrow(std::size_t n, std::size_t align) noexcept {
    try { return allocAlignedOrThrow(n, align); } catch (...) { return nullptr; }
}

void release(void* p) noexcept {
    noteFree(p);
    std::free(p);
}

}  // namespace

namespace cd::test {

RtTrapScope::RtTrapScope() noexcept : start_{tAllocs, tFrees, tLocks, tConds, tIo} { ++tDepth; }
RtTrapScope::~RtTrapScope() { --tDepth; }
RtTrapCounts RtTrapScope::counts() const noexcept { return {tAllocs - start_.allocations, tFrees - start_.deallocations, tLocks - start_.mutexLocks,
            tConds - start_.condWaits, tIo - start_.ioCalls}; }
bool rtTrapActive() noexcept { return tDepth > 0; }
namespace detail {
void noteLock() noexcept { if (tDepth > 0) ++tLocks; }
void noteCondWait() noexcept { if (tDepth > 0) ++tConds; }
void noteIo() noexcept { if (tDepth > 0) ++tIo; }
}

}  // namespace cd::test

// ---- Replaceable global allocation functions ([new.delete]) -----------------------------------
void* operator new(std::size_t n) { return allocOrThrow(n); }
void* operator new[](std::size_t n) { return allocOrThrow(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return allocNothrow(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return allocNothrow(n); }
void* operator new(std::size_t n, std::align_val_t a) { return allocAlignedOrThrow(n, std::size_t(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return allocAlignedOrThrow(n, std::size_t(a)); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return allocAlignedNothrow(n, std::size_t(a)); }
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return allocAlignedNothrow(n, std::size_t(a)); }

void operator delete(void* p) noexcept { release(p); }
void operator delete[](void* p) noexcept { release(p); }
void operator delete(void* p, std::size_t) noexcept { release(p); }
void operator delete[](void* p, std::size_t) noexcept { release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { release(p); }
void operator delete(void* p, std::align_val_t) noexcept { release(p); }
void operator delete[](void* p, std::align_val_t) noexcept { release(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { release(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { release(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { release(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { release(p); }
