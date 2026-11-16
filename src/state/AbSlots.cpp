// CircuitDrift state: A/B slot mixer implementation. See AbSlots.h. Owner: cd_state.
// Audio-side functions are noexcept, bounded and allocation/lock free.
#include "state/AbSlots.h"
#include <algorithm>
#include <cmath>

namespace cd::state {

AbMixer::AbMixer(std::size_t commandCapacity) : commands_(commandCapacity) {}

bool AbMixer::prepare(double sampleRate) noexcept {
    if (!std::isfinite(sampleRate) || sampleRate < 1000.0 || sampleRate > 1.0e6) return false;
    step_ = 1.0 / (kFadeSeconds * sampleRate);
    pos_ = target_ = 0;
    physA_ = 0;
    posMirror_.store(0, std::memory_order_relaxed);
    physAMirror_.store(0, std::memory_order_relaxed);
    return true;
}

bool AbMixer::setPosition(double x) noexcept {
    if (!std::isfinite(x) || x < 0.0 || x > 1.0) return false;
    return commands_.tryPush({AbCommandKind::SetPosition, x});
}
bool AbMixer::toggle() noexcept { return commands_.tryPush({AbCommandKind::Toggle, 0}); }
bool AbMixer::swap() noexcept { return commands_.tryPush({AbCommandKind::Swap, 0}); }
bool AbMixer::copyAToB() noexcept { return commands_.tryPush({AbCommandKind::CopyAToB, 0}); }

AbBlockActions AbMixer::beginBlock() noexcept {
    AbBlockActions act;
    AbCommand c;
    for (std::size_t i = 0; i < commands_.capacity() && commands_.tryPop(c); ++i) {
        bool structural = false;
        switch (c.kind) {
            case AbCommandKind::SetPosition: target_ = c.value; break;
            case AbCommandKind::Toggle: target_ = target_ < 0.5 ? 1.0 : 0.0; break;
            case AbCommandKind::Swap:
                physA_ ^= 1u;
                pos_ = 1.0 - pos_;
                target_ = 1.0 - target_;
                act.swapped = structural = true;
                break;
            case AbCommandKind::CopyAToB: act.copyAToB = structural = true; break;
        }
        if (structural) break;  // one structural operation per block boundary
    }
    physAMirror_.store(physA_, std::memory_order_relaxed);
    return act;
}

void AbMixer::mix(const double* a, const double* b, double* out, std::uint32_t n) noexcept {
    if (n == 0 || !a || !b || !out) return;
    for (std::uint32_t i = 0; i < n; ++i) {
        if (pos_ != target_) pos_ += std::clamp(target_ - pos_, -step_, step_);
        const double av = a[i], bv = b[i];
        out[i] = pos_ == 1.0 ? bv : av + pos_ * (bv - av);  // exact endpoints and identical slots
    }
    posMirror_.store(pos_, std::memory_order_relaxed);
}

}  // namespace cd::state
