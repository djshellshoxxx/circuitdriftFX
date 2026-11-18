// CircuitDrift control: sample-index and fractional control-tick clock (P04; spec 01 "sample counter").
// Advanced only by supplied sample frames, never by wall time or UI frames. A zero-frame call
// advances nothing. Tick bookkeeping is exact integer arithmetic (rational remainder) so the tick
// count and remainder after N total frames are identical for every block partition.
// Limits (honest): sample rate must be an integer number of Hz in [1, 2^20]; tickHz in
// [1, sampleRateHz]. Non-integer host rates are rejected by prepare(); callers must decide policy.
// Ownership: audio-thread owned. prepare()/restore() on the audio thread or while audio is stopped.
// Threading: noexcept, no allocation.
#pragma once
#include <cstdint>

namespace cd::control {

class SampleClock {
public:
    static constexpr std::uint32_t kMaxSampleRateHz = 1u << 20;

    // Resets index and remainder to zero. Returns false and leaves the clock unchanged on bad input.
    bool prepare(std::uint32_t sampleRateHz, std::uint32_t tickHz) noexcept {
        if (sampleRateHz == 0 || sampleRateHz > kMaxSampleRateHz || tickHz == 0 || tickHz > sampleRateHz)
            return false;
        rate_ = sampleRateHz;
        tick_ = tickHz;
        index_ = 0;
        rem_ = 0;
        return true;
    }
    bool prepared() const noexcept { return rate_ != 0; }
    std::uint32_t sampleRateHz() const noexcept { return rate_; }
    std::uint32_t tickHz() const noexcept { return tick_; }

    // Advances by `frames` host samples; returns the number of control ticks completed.
    // The tick phase is remainder()/sampleRateHz() of a tick (remainder in [0, sampleRateHz)).
    std::uint64_t advance(std::uint32_t frames) noexcept {
        index_ += frames;
        if (rate_ == 0) return 0;
        const std::uint64_t acc = rem_ + static_cast<std::uint64_t>(frames) * tick_;
        rem_ = acc % rate_;
        return acc / rate_;
    }
    std::uint64_t sampleIndex() const noexcept { return index_; }
    std::uint64_t remainder() const noexcept { return rem_; }

    // A saved clock is restorable only for the same prepared sample rate and a valid remainder.
    bool canRestore(std::uint32_t sampleRateHz, std::uint64_t remainder) const noexcept {
        return rate_ != 0 && sampleRateHz == rate_ && remainder < rate_;
    }
    bool restore(std::uint64_t sampleIndex, std::uint32_t sampleRateHz, std::uint64_t remainder) noexcept {
        if (!canRestore(sampleRateHz, remainder)) return false;
        index_ = sampleIndex;
        rem_ = remainder;
        return true;
    }

private:
    std::uint32_t rate_ = 0, tick_ = 0;
    std::uint64_t index_ = 0, rem_ = 0;
};

}  // namespace cd::control
