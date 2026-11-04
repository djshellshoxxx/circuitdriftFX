// CircuitDrift tests: thread-local realtime trap scope (UX-09 instrumentation, test-only).
// Owner: runtime agent. RtTrap.cpp replaces every global operator new/delete overload (plain,
// array, sized, nothrow, aligned) with a pure pass-through to malloc/posix_memalign/free. While an
// RtTrapScope is alive on a thread, allocations and deallocations made BY THAT THREAD are counted;
// other threads are never counted and nothing is logged. Outside any scope the replacement is
// inert. ASan intercepts the underlying malloc/free, so leak/overflow checks still apply.
// Limits: counts operator new/delete only (not raw malloc, mutex locks or OS I/O); those traps are
// not implemented. A bounded test run is not the one-hour UX-09 stress qualification.
// Linked into every test executable via tests/support/*.cpp.
#pragma once

namespace cd::test {

struct RtTrapCounts {
    long allocations = 0;
    long deallocations = 0;
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

}  // namespace cd::test
