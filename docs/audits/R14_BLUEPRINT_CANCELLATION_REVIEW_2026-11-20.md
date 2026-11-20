# R14 blueprint cancellation and worker review — 2026-11-20

## Compiler prerequisite

Implementation commit: `e23f2404bb9ce3856a547a3e9ae323728f3cd0ea`, based on
`c6ebb22daab9c46edffa761b21f3fd45320e5074`.

`compile` and `compileText` now accept a final optional `std::stop_token`, after
generation. Existing callers retain the default behavior. `DiagCode::Cancelled`
is appended to preserve existing diagnostic values. Cancellation is checked
before parsing/compilation, between compiler stages, before DC preparation, at
every numerical probe sample, and before a successful graph result is returned.
Cancellation returns no graph and retains the partial cost report. Solver and
audio behavior are unchanged.

The focused `blueprint_compile` suite passed under GCC 14.2 with `-O1`, ASan,
UBSan, and float-cast-overflow instrumentation. Build parallelism was one.
CTest reported 1/1 passed in 3.02 seconds. Leak detection was disabled because
the local tracing environment cannot run LSan; this run does not qualify leaks.

New coverage checks pre-cancelled text and typed calls, cancellation precedence
over malformed/config-invalid input, normal acceptance with a live token,
immutability of an already returned graph, and interruption of a 62-component,
33-node probe at 384 kHz and 16x oversampling. The interruption returned check 5,
with completed DC and probe Newton iterations, within the test's five-second
shutdown ceiling. The existing 2,000-mutant suite also passed: 17 accepted and
1,983 rejected. No existing acceptance threshold was reduced.

## Independent source review

Reviewed the root checkout's uncommitted worker/bridge changes on base
`2254d9e99e7dbc5c6046c348a965a284afbc4f44`, including the subsequently added
`cancel()` and `cancelBlueprintValidation()` methods. The reviewed blobs were:

| File | Blob |
| --- | --- |
| `plugin/src/ui/BlueprintValidationService.h` | `bfbe0862bc20a5059e4134a4b2d24885ca683bfc` |
| `plugin/src/ui/BlueprintValidationService.cpp` | `8e5e7cbe03c7965f3fa28b28d9db8629812b2bb3` |
| `plugin/src/ui/ProcessorEditorBridge.h` | `3c070b2c264b7f070780c0f2d6e23aec2c16d9c1` |
| `plugin/src/ui/ProcessorEditorBridge.cpp` | `1c39344bd81550bfbbba6faeec4b4e933cb63fbe` |
| `plugin/src/ui/EditorBridge.h` | `2d8da76a4e01a0b6f51f16b686ce32368c9af1c1` |

No blocking defect was found in these source snapshots:

- Request replacement, cancellation, rejection, and report publication share a
  mutex. A worker publishes only when its own stop source is still live, so an
  older completion cannot replace a newer request/rejection or return after a
  completed cancellation.
- The service retains at most one active and one pending document, each admitted
  at the 1 MiB limit. The bridge checks UTF-8 size before making its worker copy.
- Destruction stops active work, drops pending work, wakes the worker, and joins
  it before service members are destroyed. Ordinary compiler exceptions become
  failure reports. Thread creation failure leaves a report and permits retry.
- Worker inputs are owned JSON, configuration, and synthetic fixtures. The
  worker does not access the processor or audio runtime. Compiled graphs are
  destroyed on that worker and are never installed.
- The bridge rejects a pending report if its recorded host-state epoch or
  observed sample rate/oversampling/preview context changed before delivery.
  Live application remains explicitly unavailable.

Panel integration must call cancellation when text changes, reloads, or the
panel closes, because the processor-owned bridge outlives the editor. Reports
must display their recorded sample rate, oversampling, and synthetic/preview
scope; numerical validation does not establish physical accuracy or current
audio installation. These integration notes were sent to the coordinator.

This is source review of the listed worker/bridge blobs plus the focused core
test above. Native worker/panel regression tests and cross-platform CI remain
the coordinator's qualification work.
