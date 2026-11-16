// CircuitDrift state: A/B slot mixer (CD-F24, 05-state "State ownership and A/B").
// Owner: cd_state.
// Both slot engines run continuously on identical input; this mixer combines their prepared
// outputs. Crossfade is equal-sum linear, evaluated as out = a + x*(b - a), so identical slot
// outputs are reproduced bit-exactly at every position x (unity, 0 dB error). The A/B toggle
// moves the endpoint with a 20 ms linear ramp; continuous position changes use the same slew
// (full range per 20 ms), so |dx| per sample <= 1/(0.02*Fs).
// Swap exchanges which physical engine is "A" (identity, configuration and dynamic state move
// with the engine) and complements position/target in the same sample, keeping the output
// unchanged up to rounding. Copy A->B is reported to the caller, which copies the physical A
// engine's configuration and dynamic snapshot (including seeds) into B at that block boundary.
// Threading: one message thread enqueues commands (setPosition/toggle/swap/copyAToB) through a
// preallocated SPSC queue; the audio thread calls beginBlock()/mix(). At most one structural
// command (Swap or CopyAToB) is applied per block so their order is deterministic. Audio APIs
// are noexcept and allocation/lock free. prepare() is non-audio.
#pragma once
#include <atomic>
#include <cstdint>
#include "state/RtQueue.h"

namespace cd::state {

enum class AbCommandKind : std::uint8_t { SetPosition, Toggle, Swap, CopyAToB };
struct AbCommand { AbCommandKind kind = AbCommandKind::SetPosition; double value = 0; };

struct AbBlockActions {
    bool swapped = false;   // physicalA() changed this block (already applied)
    bool copyAToB = false;  // caller must copy engine[physicalA()] -> engine[physicalB()] now
};

class AbMixer {
public:
    static constexpr double kFadeSeconds = 0.020;
    explicit AbMixer(std::size_t commandCapacity = 64);  // allocates

    // Non-audio. Resets to position 0 (pure A), physical engine 0 as A. False on invalid Fs.
    bool prepare(double sampleRate) noexcept;

    // ---- message thread (single producer); false when invalid or the queue is full ----
    bool setPosition(double x) noexcept;  // x in [0,1], finite
    bool toggle() noexcept;
    bool swap() noexcept;
    bool copyAToB() noexcept;

    // ---- audio thread ----
    AbBlockActions beginBlock() noexcept;
    std::uint8_t physicalA() const noexcept { return physA_; }
    std::uint8_t physicalB() const noexcept { return std::uint8_t(physA_ ^ 1u); }
    // a: output of slot A (engine physicalA()), b: slot B. out may alias a or b. n may be 0.
    void mix(const double* a, const double* b, double* out, std::uint32_t n) noexcept;
    double position() const noexcept { return pos_; }
    double target() const noexcept { return target_; }
    double stepPerSample() const noexcept { return step_; }

    // ---- any thread (GUI mirror, relaxed) ----
    double positionMirror() const noexcept { return posMirror_.load(std::memory_order_relaxed); }
    std::uint8_t physicalAMirror() const noexcept { return physAMirror_.load(std::memory_order_relaxed); }

private:
    SpscQueue<AbCommand> commands_;
    double step_ = 0;
    double pos_ = 0, target_ = 0;
    std::uint8_t physA_ = 0;
    std::atomic<double> posMirror_{0};
    std::atomic<std::uint8_t> physAMirror_{0};
};

}  // namespace cd::state
