# GIF preintegration review — round 19, 2026-11-20

## Scope and source

Three fresh workers completed independent code, bug and defensive-security
reviews of `codex/cdfx-gif-capture-20261120-r14` at
`ecae510b72c0ef4327bacfb6e4aa35d08eea2748`, tree
`4505101823929a1cb388fde5893f8358290188eb`. That branch contains a real local
GIF implementation, derived from c6ebb22, and remains preserved. It has not yet
been integrated or qualified against the current consolidated source.

The review was source inspection and focused diff analysis. No worker executed
tests, fuzzing, network requests, exploit reproduction or a production service.
Existing direct-bridge tests submit three flat frames and do not establish the
actual editor surface, visible/hidden timing or complete resource budget.

## Findings handed to repair owners

| Area | Finding and required behavior |
| --- | --- |
| Terminal drain | The worker observes an empty queue before reading stop. A producer can publish its final frame and stop between those observations, leaving that frame unconsumed. Establish producer admission/termination ordering and observe producer completion before terminal empty-queue detection. Cover a final distinct frame and a deadline during producer activity. |
| Independent duration | CapturePanel refresh supplies frames and notices duration expiry, while EditorCore refreshes only the visible drawer. Switching drawers or hiding the editor can leave a session alive indefinitely. The backend needs its own monotonic deadline; view changes must not own session termination. |
| Output timebase | A 50 ms last-now threshold driven by a roughly 30 Hz UI loop yields roughly 15 submissions/s, while each encoded image claims 50 ms. Submitted sequence numbers cannot represent missing UI intervals. Use absolute 20 Hz timeline indices and repeat a retained encoded frame for missed intervals. |
| Captured surface | getTopLevelComponent snapshots can capture the capture form when a narrow drawer hides the visual pane, or include embedding-host components. Render an explicitly owned EditorCore visual surface with bounded software image storage. |
| Declared dimensions | The branch uses 360×640, 480×480 and 640×360. The interface/export contract declares 540×960, 540×540 and 960×540. Use one shared dimension mapping and exercise each aspect. |
| Cumulative allocations | A 32 MiB encoded vector copied into a 32 MiB string already exceeds the 64 MiB capture allowance once the queue and last image are included. Admit encoder capacity, concurrent scratch and all caller image/queue buffers before capture. Save bytes without a full output copy. Arithmetic bounds on payload are not measured whole-process RSS or GPU overhead. |
| Exception handling | A noexcept worker catch constructs an allocating string before entering the protected failure helper. A second allocation failure can terminate the process. Pass allocation-free reasons and protect best-effort descriptive storage inside the helper; apply the same rule to producer failures. |
| Installation outcome | Allocating a Saved message after atomic replacement can throw and report Failed despite successful output installation. Publish Completed irreversibly after the atomic installation and make descriptive text best effort. |
| Encoder byte limit | addFrame checks size after append and finish appends a trailer without a limit check. Bound every append before mutation, reserve admitted capacity, and include the final trailer. |
| Local feedback | Every panel refresh replaces its own PNG/start-rejection message with generic session status. Preserve the action result until a relevant user action or capture-state change supersedes it. |

Ownership, basic request admission and trusted-directory atomic replacement were
coherent in the inspected source. Review did not find a separate destination
clobber or use-after-free defect. That limited finding is not a whole-repository
security signoff, an allocation-failure test result or a real-host qualification.

## Fresh round-20 handoff

All three R19 workers finished before fresh repair workers started in isolated
worktrees based on `8ff0adfea571e977adec128622209f63bf997d1d`:

- **Backend:** GifCaptureSession, capture-only bridge API/integration and a native
  regression header. Own the injectable monotonic clock, absolute frame requests,
  independent deadline, producer/stop/drain ordering, bounded admission and
  terminal status. Preserve the consolidated validator and preset worker.
- **UI:** ServicesPanels, EditorCore capture surface/pump and shell regressions.
  Use explicit owned software rendering at the declared dimensions; handle
  narrow layout, embedding-host exclusion, drawer changes and retained feedback.
  Do not duplicate backend ownership or borrow callbacks across component lives.
- **Resources:** GifEncoder, a zero-copy atomic byte-writer adapter and portable
  media/state regressions. Reserve and bound all output/scratch capacities,
  include the trailer, and reuse the last encoded image block for timeline gaps.

The integration owner owns compilation and execution after the patches are
combined. Required evidence includes decoded dimensions/frame timing, final
distinct-frame retention, manual and independent automatic stop, hidden/idle
expiry, restart, byte boundaries, atomic output and the actual editor surface.
Fresh successors should inspect the combined source and test evidence.

The declared memory calculation must describe admitted buffer/workspace payload.
Allocator bookkeeping, platform/host/GPU storage and whole-process peak RSS
still require profiling under G11. MP4, physical references and release gates
remain separate incomplete work.
