> Historical branch record preserved from ecae510b72c0ef4327bacfb6e4aa35d08eea2748. The [R19 review](GIF_PREINTEGRATION_REVIEW_2026-11-20_R19.md) identifies additional defects and assigns fresh repairs. This original record does not qualify the subsequent integrated implementation.

# Silent GIF capture: production bridge handoff — 2026-11-20

## Exact source and scope

The coordinator draft PR #16 was advanced with a guarded, **non-forced fast-forward** from `b3e05b652147a584ad5fb22e5715d1e61b702318` to `c6ebb22daab9c46edffa761b21f3fd45320e5074`. The latter is the full descendant of child PRs #17–#25 and passed its own [exact-source CI #44764678463](https://github.com/djshellshoxxx/circuitdriftFX/actions/runs/44764678463): Linux sanitized tests, Windows builds/tests and VST3 pluginval strictness 5. The parent retains its original branches and `main` is not changed. Source review does **not** imply independent code/bug/security worker signoff.

This isolated continuation, `codex/cdfx-gif-capture-20261120-r14`, is based on coordinator commit `c6ebb22d`. It wires a real message-thread capture service to `ProcessorEditorBridge` and the existing Capture panel's editor-only snapshot submissions. Only silent GIFs are supported; PNG remains atomic and MP4 remains explicitly unavailable. There is no audio tap or audio-thread work.

## Implementation contract

- `GifCaptureSession` preallocates a three-slot `media::CaptureQueue` with a 16 MiB queue ceiling; fixed RGBA canvas sizes are 360×640, 480×480 or 640×360. Each submit copies an editor-only `juce::Image` into the queue on the message thread. Invalid dimensions fail explicitly.
- Worker-only `media::GifWriter` writes 20 FPS, no audio, 1–15 seconds, maximum 300 frames and 32 MiB output. A dropped unclaimed queue frame is represented by repeating the latest encoded frame; the producer never waits on the encoder.
- `stopCapture` signals a graceful drain/finalize. `CaptureStatus` reports current state, submitted/encoded progress, written/repeated frames, errors and completion. The session owner joins the worker on destruction; replacing a finished capture joins the previous worker.
- GIF bytes are fully encoded in memory before the existing same-directory `saveStateFileAtomic` writes and atomically replaces the requested local file. A codec/write error never truncates a previously saved destination. Trusted destination directories and durability after power loss remain documented boundaries.
- The Capture panel already supports destination selection and per-frame editor snapshot polling. It now hands the worker exact dimensions instead of a mismatched display-size crop.

## Added regression assertions

The real JUCE bridge test in `plugin/tests/ui/snap_live_bridge.cpp` exercises start, three frames at 480×480, explicit stop, bounded completion polling, GIF89a header and trailer, expected frame counts and Completed progress; it rejects audio-in-GIF and overlong requests, and verifies an invalid-size failure does not modify an existing GIF. Media codec tests in `tests/media/test_media.cpp` independently decode GIF frames and verify quantization, limits, determinism and queue ordering. **The new source needs its own native/CI execution.** Do not attribute parent #25 success to this new implementation.

## Review ownership and outstanding gates

Code review: check JUCE image conversion and UI-thread ownership, worker lifetime, submitted/encoded sequence and finalization. Bug hunt: test stop before first frame, queue saturation, failure after a completed GIF and repeated starts. Defensive security: check destination symlink/trusted-directory assumptions, no-overwrite expectation (this capture intentionally uses replace), bounded encoder memory, async exceptions and concurrent destruction. Independent fresh reviewer workers were **not exposed** in the current connector; these are explicit unclaimed review gates.

Exact-child-head Linux CTest and Windows/pluginval, interactive DAW capture, sustained UI and disk performance, physical model G01 and measured G03/G04 qualification remain open until executed. Full G12 runtime/control/seed/session checkpoint capture, dynamic A/B, live graph Apply/fault/restoration, provider integration and PR #9/#10/#14 semantic mapping remain separate. No release, deployment, destructive ref rewrite or original-branch deletion.
