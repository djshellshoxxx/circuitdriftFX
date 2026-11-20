# Host-state publication and G12 handoff — round 25

## Scope and result

A fresh bug-hunt worker reviewed source
`b84d2ba119c03f079cd74886cbd066a2da6013f6` after the complete R24 set.
This was source inspection only: no builds, application runs, source changes or
new regression executions. Root independently inspected the current adapter and
the pinned JUCE state/Standalone call sites. One current P2 defect needs repair;
full dynamic-session capture remains unfinished rather than newly regressed.

## P2: valid host restore can expose a partial parameter configuration

[PluginProcessor.cpp](../../plugin/src/PluginProcessor.cpp) validates the entire
incoming state before calling APVTS replaceState. This correctly rejects an
invalid candidate before applying its parameters. For a valid candidate, JUCE
updates one child parameter at a time. The adapter's parameterSnapshot reads
individual raw atomics while processAudio continues, so one callback can observe
old values for some parameters and restored values for others. The editor-state
epoch advances after replacement and is not an audio snapshot guard.

The pinned JUCE revision is `dabd5ad13cd869cba41046947fffc8e5ce5cd662`.
Its AudioProcessorValueTreeState::updateParameterConnectionsToChildTrees calls
setNewState for each child separately. Its Standalone load-state callback calls
processor->setStateInformation directly after reading the selected file, without
suspending audio. This establishes a shipped reachable interleaving. No measured
audible artifact or deterministic execution was claimed by the audit.

### Bounded repair contract

Keep parsing/validation and publisher serialization off audio. Audio must retain
its preceding complete configuration while replacement is incomplete and adopt
a complete accepted configuration at a callback boundary, including zero-frame
callbacks. Ordinary automation must resume normally. Multiple host-side publishers
must be serialized explicitly; an unqualified SPSC assumption is insufficient.
Preserve Full/LITE/legacy compatibility, invalid-input rejection, UI state and
staged latency behavior. Audio may neither wait for the publisher nor allocate,
lock, log or perform I/O. Include LITE's separate bypass parameter in coherence.

Ownership is limited to PluginProcessor.h/.cpp and the native parameter regression
suite, optionally a focused regression header. A test-owned APVTS listener can
pause replacement after one changed parameter and before another. During that
pause, a callback must retain the complete old target pair. After release, it
must adopt the complete new pair. Cover stopped/unprepared restore, immediate
save, successive/rejected restores, automation, lifecycle generation and allocation
invariants. Record the unchanged-source failure if the fixture can be run before
the repair; do not infer a pass from source inspection.

## Full-session prerequisites

| Area | Existing implementation | Remaining contract |
| --- | --- | --- |
| Host state | APVTS parameters, adapter/edition versions, bounded UI settings | Runtime/control/engine state, instance/slot seeds and graph identity |
| Engine snapshot | Preallocated allocation/capture, layout identity, validated restore and rollback | Runtime ownership and complete controller state |
| Controls | CreativeDrift has a fixed-size validated snapshot | Tempo/key/MIDI/transient/sidechain/quantizer/harmonic state, cached zero-frame outputs |
| Canonical config | Config and slot metadata, parameter conversion, seed checks and deltas | Authoritative adapter config plus coherent transaction |
| Persistence | Strict StateDocument, config-only session helpers and atomic file writer | Versioned bounded complete checkpoint envelope |
| A/B and sound presets | Parameter copy/swap; catalog/import/export/delete | Slot dynamics/seed/blueprint transaction and ownership of shared downstream state |

The [state/preset contract](../planned-features/05-state-presets-community.md)
requires continuous independent histories, dynamic copy/swap, a preallocated
block-boundary snapshot and immediate host saves using the last coherent state
without waiting for stopped audio. Production sound Load/Save is explicitly
disabled in [ProcessorEditorBridge.cpp](../../plugin/src/ui/ProcessorEditorBridge.cpp),
so missing dynamic capture is not asserted as a defect of an enabled preset path.

### Checkpoint shape and admission

[EngineTypes.h](../../src/engine/EngineTypes.h) describes engine layout, sample
index, an all-state values vector and an integer/RNG vector.
[StateDocument.h](../../src/state/StateDocument.h) instead describes model/Fs/OS
identity plus electrical and thermal vectors; its reader/restore policy caps each
floating vector at 4,096 entries and RNG at 64. At 48 kHz, source-derived engine
history/prehistory storage alone totals 4,096 doubles and pitch buffers add 2,048,
before other state. Copying all engine values into electrical cannot satisfy that
existing bound. This is a source-derived lower bound, not a measured checkpoint.
Use an explicitly versioned/admitted representation; preserve current limits and
compatibility until that new contract is implemented.

[Preset.cpp](../../src/state/Preset.cpp) deliberately rejects checkpoint/UI fields
in configFromSessionJson. A complete-session adapter must reuse shared config
validation through an explicit checkpoint layer, not silently expand this helper.
[Processor.h](../../src/runtime/Processor.h) currently exposes no composed runtime
snapshot. Existing engine continuation tests with external fixed modulation do
not establish runtime-plus-control continuation.

### Worker ownership and later sequencing

Audio-pitch state spans the producer ring, worker resampling/window/acquisition
and published results. Audio cannot copy the worker-owned object concurrently.
A first runtime snapshot may explicitly report unsupported audio-pitch checkpoint
capture and save configuration only; restoring only the latest PitchResult must
not be presented as exact continuation. Persist harmonic map/model identity, never
a borrowed pointer value.

After coherent host publication: implement validated fixed-size control snapshots;
compose engine/control preparation identity and seeds with atomic rollback;
implement bounded persisted/published snapshots and stopped-audio handoff; then
connect complete sound presets and dynamic A/B. Whole-engine checkpoints alone
do not define slot copy/swap or shared downstream/crossfade ownership. G12/T24
remain open until those integrated behaviors are executed and mismatch fallback
is visible.
