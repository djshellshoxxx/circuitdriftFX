# Blueprint validation freshness review — 2026-11-20

Reviewed feature `333760c438a1ddf638dccdfa1be44b38788e43a4` at integration
base `1bfc6f89c8849cf3d998ca01fa31016eb8f7de14`. Scope: the new validation
worker, compiler cancellation, live bridge validation, blueprint panel, and
their existing regressions. This is a scoped code review, not a whole-repository
security or host qualification.

## Finding and repair

**P2: a delivered validation report stayed valid after its inputs changed.**
At the reviewed base, `LifecyclePanels.cpp:414–423` only polled reports while
Validating. After entering Ready, sample-rate changes, active oversampling
changes, host-session replacement and audio release left the old Valid/Ready
display untouched. The live bridge checked configuration/session freshness only
when consuming a report. The text-change handler at line 391 changed the state
to Draft but retained the previous green Valid report.

The repair exposes the existing host-session epoch in `StatusInfo`, captures
sample rate, active oversampling and epoch when validation starts, and checks
that identity during subsequent panel refreshes. A changed document or
configuration clears its report and returns to Draft with an explicit request
to validate again. Invalidation preserves the editor text. Ordinary parameter
edits do not increment the host-session epoch. Live graph application remains
unavailable.

## Regression evidence

The new `snap_live_bridge.cpp` regression constructs the actual panel with
`ProcessorEditorBridge`, compiles a supported synthetic RC fixture, then checks
the displayed state after text edits, sample-rate changes, active oversampling
changes, same-configuration session replacement and audio release. Successful
explicit revalidation is checked between transitions. It also checks that an
unchanged configuration retains the preview and that Apply stays disabled.

| Execution | Result |
| --- | --- |
| Unchanged production at the base above, with only the new test | **Failed**, exactly the five stale-report assertions; 1.32 s test / 1.33 s total |
| Production repair, identical test source | **Passed**, `ui_snap_live_bridge` 1/1; 1.56 s test / 1.57 s total |
| Patch whitespace validation | `git diff --check` passed |

Native environment: Linux, Clang 19.1.7, C++20, Custom `-O0 -g0 -DNDEBUG`,
sanitizers off, and the repository's pinned JUCE sources. The focused CTest
selection was `-C Custom --output-on-failure -R '^ui_snap_live_bridge$'`.
Two build attempts stopped with compiler exit 255 and no C++ error diagnostic;
unchanged incremental retries completed, with the final retry bypassing the
compiler cache. Neither the test source nor its assertions were changed between
the failed and passing executions.

## Worker, lifetime and reporting review

No additional concrete defect was found in the reviewed worker ownership path:
pending/completed requests and latest generation are mutex-serialized;
superseding a request stops its token; only the latest generation may publish;
destruction cancels and joins before releasing the registry; and compiled
candidate storage is destroyed on the worker. Cancellation is checked before
compilation and at every electrical probe sample. Parsing and DC preparation
remain bounded operations without internal cancellation points.

The existing supersession and teardown tests do not deterministically establish
that compilation has entered an active probe before cancellation. They are
functional checks, not teardown-latency qualification. Reports retain synthetic
model provenance and analytical operations-per-electrical-sample estimates,
without claiming measured CPU time. No DAW, physical-model, ThreadSanitizer or
production-performance qualification follows from this review or native test.

The repair changes only `EditorBridge.h`, `ProcessorEditorBridge::status()`,
the blueprint panel, and validation tests. Preset catalog methods and worker/
compiler production code are unchanged. Combined-head integration testing is
still required after merging concurrent repairs.
