// CircuitDrift state: named A/B operations (CD-F24). See ConfigDelta.h for exact semantics.
// Each returns the ONE ConfigDelta the adapter applies to the host parameters as one undoable
// action (an empty delta means the operation changes nothing). Message/worker thread only.
#pragma once
#include "state/ConfigDelta.h"

namespace cd::state {

inline ConfigDelta copyAtoB(const Config& c) { return makeABDelta(c, ABOp::CopyAtoB); }
inline ConfigDelta copyBtoA(const Config& c) { return makeABDelta(c, ABOp::CopyBtoA); }
// mirrorCrossfade=true (default): ab.crossfade -> 1 - ab.crossfade so the mixed sound is unchanged.
inline ConfigDelta swapAB(const Config& c, bool mirrorCrossfade = true) { return makeABDelta(c, ABOp::Swap, mirrorCrossfade); }

}  // namespace cd::state
