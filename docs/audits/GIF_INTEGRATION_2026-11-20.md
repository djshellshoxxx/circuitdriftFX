# GIF integration and successor reviews — 2026-11-20

## Sources and completed repair set

The [R19 review](GIF_PREINTEGRATION_REVIEW_2026-11-20_R19.md) completed three
independent reviews of original GIF branch ecae510. Three fresh R20 workers
repaired the backend, UI and resources in isolated worktrees from 8ff0adf.
Root merge 5e7057e preserves the original branch history and its
[historical audit](GIF_CAPTURE_SERVICE_2026-11-20.md). Reviewed source follows
that merge without replacing the consolidated validator or preset services.

| Owner | Worker commit | Integrated commit | Result |
| --- | --- | --- | --- |
| Resources | 68deeee | eb97e54 | Reserved and bounded encoder capacity/scratch, encoded-image repetition, no-copy atomic byte output and portable boundary tests. |
| Backend | 168eb29 | d083d38 | Independent deadline, absolute frame indices, producer/drain ordering, finite stop endpoints, failure containment and native tests. |
| UI | afd0633 | 3d73b2d | Owned software rendering, exact dimensions, shell frame pumping, retained action feedback and real-editor tests. |
| UI follow-up | 67bd915 | 50fcb0e | Contains allocating status/stop failures during editor destruction; one stop attempt. |
| Root test hook | — | 7aa5654 | Executes backend regressions in live_bridge; UI worker hooked shell regressions. |
| Backend follow-up | a425b3f | c012622 | Discards stale Recording/Finalizing diagnostics after a state transition. |

Initial R21 source: **c0126227f3822268e725bb7f6daf7392153f0d0b**,
tree **2b25c748440c9fd7e8bd323944d662a9373a9fee**. R20 workers performed
source/diff checks only. Root owns executed qualification.

## Implemented behavior and limits

The session owns an immutable request and a finite 1–15 s monotonic 20 Hz
timeline. Each editor opportunity requests at most one absolute frame index.
The worker repeats an encoded image block for missed intervals, retaining a
five-centisecond delay per image. A late first image fills its missing prefix
and counts as repetition. Queue sequence numbers are not the timebase.

Automatic expiry does not depend on panel refresh. An issued but unsubmitted
token cannot keep recording alive. Manual stop uses elapsed slots and the
highest admitted image, with at most one 50 ms rounding slot; it does not pad
to the requested maximum. Producer admission and the active-producer gate
precede terminal empty-queue observation, preserving the final publication.
Restart releases old buffers and changes the epoch before accepting new tokens.

EditorCore renders only its selected Easy/Workbench subtree directly into a
declared-size software image. A narrow drawer may cover that subtree without
replacing the source; covered Workbench telemetry still updates. Parents,
headers and host siblings are not traversed. GIF sizes are 540×960, 540×540 and
960×540; PNG sizes are 1080×1920, 1080×1080 and 1920×1080. MP4 has no fallback
canvas and remains unavailable.

The shell produces frames independently of the selected drawer. Hidden editors
stop painting while the worker repeats the last image to its deadline.
Destruction requests finalization before releasing surfaces and contains
status/stop exceptions. No renderer callback is borrowed by the worker. PNG
resolves only a registered core among the panel's actual ancestors.

Local PNG/rejected-start feedback survives unchanged session polls. Failure
reasons have an allocation-free representation; descriptive storage is best
effort. Atomic installation follows complete encoding. Completed is published
immediately after installation and cannot be reversed by message allocation.

The writer reserves output once, checks each append and trailer before mutation,
rolls back a partial frame on failure, and transfers output only on success.
The atomic writer borrows the byte view synchronously. At the largest canvas,
encoder/output/scratch is 43,016,704 bytes; eight RGBA allowances add 16,588,800;
a 1,048,576-byte margin gives **60,654,080 bytes** against **67,108,864 bytes**
admission. This is buffer/workspace payload, not whole-process RSS or allocator,
host/platform/GPU memory qualification.

## Executed verification

At eb97e54, rebuilt media, state_safety and state_preset passed **3/3 in 6.03 s**,
GCC 14.2 ASan/UBSan/float-cast-overflow. Production/most tests were O1; the large
state_safety test TU was O0 after the previously recorded resource limit. Leak
detection was disabled under the tracer. Portable inputs are unchanged through
c012622. Tests cover small-file byte boundaries, larger LZW subblocks, trailer
admission, caller-output preservation, repeated global/local/quantized images,
overflow admission and exact binary atomic writes. Preset tests retain the
no-replace forwarding coverage.

Final native source **b84d2ba119c03f079cd74886cbd066a2da6013f6**, tree
**0c2e17c0ec416991c8a69b130015b1e0aa727780**, passed **6/6 in 61.44 s**
with Clang 19.1.7 Custom O0 and pinned JUCE/CLAP, without sanitizers. This includes
all session and subsequent review repairs. Backend tests use injected time and CV barriers
for final-frame drain, deadline during an admitted producer, idle expiry,
jitter, manual stop, frame caps, exceptions and restart. Shell tests use the
production editor/worker with a synthetic clock; only destination selection is
substituted. Record actual results in the
[execution record](../evidence/gif-integration-2026-11-20-r20.json).
Publication 338e952 retains separate earlier 31/31 core and 6/6 native evidence.

## Completed R21 review and fresh R22 handoff

All three R20 workers finished before three fresh R21 reviewers inspected
c012622. R21 reviews were source-only, without builds, execution, file mutation,
fuzzing, vulnerability reproduction or external services.

- **Code audit:** P2 constructor-lifetime defect. EditorCore registers before
  throwing initialization; failed construction skips its destructor and leaves
  an entry now used by production PNG lookup. Register after successful priming;
  cover failure, healthy retry on the same host, PNG lookup and normal removal.
- **Bug hunt:** P2 decoder-oracle defect. lzwDecode stops at the expected pixel
  count without requiring EOI. Require valid termination and test missing,
  truncated, premature and width-boundary EOI. No production EOI-width defect
  was confirmed: the final equality-width increment is coherent in the inspected
  trace. Separately, the required warning above 2% repeated frames is absent.
  Cover exactly 2%, above 2% and the hidden 19/20 case. No new session-ordering
  defect was found, and both regression headers are invoked by CTest applications.
- **Defensive security:** independently corroborated the registry defect with
  code-auditor ownership retained. No additional encoder/session/queue/atomic
  writer finding. Reviewed admission, simultaneous capacities, byte/trailer
  bounds, failure containment, destination preservation, terminal success,
  software surface ownership and restart.

All R21 reviewers finished before fresh R22 UI repair, decoder repair and
defensive destination-flow workers started. R22 additionally confirmed a P2
destination-authority gap: format changes rewrite the selected extension, and
missing extensions are appended after chooser confirmation. Either can redirect
output to an unconfirmed sibling path. The UI worker implemented clear/reselect on
format change and exact-path extension validation, with cancellation/case/error
regressions. Accepted paths remain verbatim; same-format selections and chooser
cancellation preserve the current destination. Empty destinations are rejected
inside startRecording as well as through disabled UI controls.

Trusted-directory atomic-write assumptions remain. Real-DAW/stall/disk-full,
optimized performance, whole-process memory, TSan/leak and physical-reference
gates are open. Silent GIF does not claim audio synchronization, MP4 support or
completion of the full export acceptance contract.


## Completed repairs and fresh reviews — R22 through R24

| Repair | Integrated source | Verification |
| --- | --- | --- |
| Strict GIF decoder termination | aedef7b | Requires EOI and exact decoded size. Eleven independently hand-packed valid and eleven invalid fixtures, full-envelope propagation and real encoder code-width boundaries; media suite passed 1/1 in 0.57 s with ASan/UBSan/float-cast-overflow. |
| Editor registration lifetime | dbf5e3d | Registry publication follows all throwing initialization. Constructor-failure, healthy-retry and removal checks execute in shell. |
| Repetition warning | 38fd259 | Separate warning above 2%; exactly 2% is not warned. Result/failure/destination information is retained; the warning clears in a fresh session. |
| Exact destination authority | ef115d2 | Format changes require reselection. Missing/wrong suffix is rejected without rewriting the selected path. Case, cancellation and sentinel-file checks execute in shell. |
| Failed-registry test containment | f0e2101 | A failed stale-entry assertion returns before later address reuse could mask it or dereference the old object. |
| JUCE empty-file construction | a512cd7 | Explicit juce::File{} resolves two compiler ambiguities in the destination repair. The complete native build then passed. |
| First local result geometry | b84d2ba | A newly visible message triggers immediate owner layout. Regression checks positive visible bounds and wrapped height before backend fields change. |

All three R22 workers completed before fresh R23 code, bug and defensive reviewers
inspected the repaired source. R23 reported no new actionable finding in its
assigned source scopes. Root subsequently found the first-result geometry defect
and repaired it before the final native execution. A fresh R24 code reviewer
confirmed the cause, fix and meaningful regression. Review-only workers did not
claim dynamic execution.

The other R24 workers independently decoded actual output and inspected dependency
and CI configuration. All three R24 workers completed before the fresh R25 set
started. The latter set owns the two build-gate findings below and a read-only
bug hunt of the next G12 session-state feature.

## Independent output verification

Pillow 13.0.0 on Python 3.12.14 fully decoded every pixel of all **63 frames** in
four freshly generated production files. Truncated-image tolerance was disabled
and warnings were errors. Each file's mtime lies inside the final native test
window; its SHA-256, frame hashes and content metrics are recorded in the
[independent evidence](../evidence/gif-independent-2026-11-20-r24.json).

| Production fixture | Canvas | Images | Delay | Duration | Distinct decoded images |
| --- | --- | --- | --- | --- | --- |
| editor_30hz.gif | 540×540 | 20 | 50 ms each | 1,000 ms | 11 |
| editor_jitter.gif | 540×540 | 20 | 50 ms each | 1,000 ms | 6 |
| editor_hidden.gif | 540×960 | 20 | 50 ms each | 1,000 ms | 1 |
| editor_destroyed.gif | 960×540 | 3 | 50 ms each | 150 ms | 1 |

All four have infinite-loop metadata, the expected GIF89a signature and trailer,
and substantial nonuniform visual content. The destroyed editor stops after
125 ms of synthetic elapsed time; its partial slot rounds to three 50 ms images.
Hidden/destruction cases repeat their sole admitted image exactly. This establishes
compatibility with an independent decoder for these fixtures, not a formal proof
of all GIF/LZW inputs. Native tests separately enforce selected subtree pixels,
all declared canvases, foreign-host exclusion and strict EOI termination.

The [native log](../evidence/gif-native-2026-11-20-r24.log) and
[JUnit results](../evidence/gif-native-2026-11-20-r24.xml) retain all six results.
The [source map](../evidence/commit-map-gif-2026-11-20.json) records verified full
local/remote tree equality through b84d2ba. Its corresponding remote source is
7c9ed63e15fcb3167eb2c57d1aee4fdac9019d20. Original GIF branch ecae510 remains intact.

## Build-gate follow-up

R24 found two independent configuration defects at b84d2ba. The dependency
verifier checks only HEAD and runs after MakeAvailable has configured upstream
code. The release workflow still contains the old asynchronous GUI pluginval
wrapper, already corrected in CI. Neither finding demonstrates compromised
sources or a third-party exploitation route. The fresh R25 workers own source
verification before configuration, source-only caching, fresh release acquisition
and one shared synchronous validator wrapper. Their evidence is separate from
the completed GIF qualification. Exact-publication Windows/pluginval, real DAW,
optimized memory/CPU, disk/stall and complete G11 acceptance remain required.
