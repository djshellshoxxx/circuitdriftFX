# Live blueprint validation — R14, 2026-11-20

## User-visible behavior

The Full edition's Blueprint panel now validates draft JSON through the real
`cd_blueprint` compiler on a dedicated worker. Example RC inserts the documented
two-component synthetic fixture as editable text. It does not install a circuit
or start validation automatically. The panel shows schema/model/connectivity/DC/
probe/binding failures with their check, diagnostic code, component and pin.

A successful report includes the real component/node/nonlinear/unknown counts,
estimated arithmetic per electrical sample, estimated storage, sample rate and
oversampling. Arithmetic estimates are not measured CPU time. The fixture-v1
registry is explicitly synthetic. Without an available audio snapshot, the
report labels its preview configuration; an unprepared processor uses 48 kHz/1x.

Live application remains unavailable. The disabled Apply control and adjacent
explanation remain present even after successful validation. No graph is
installed, no host parameter or UI preference changes, and no undo transaction
is created by validation. The LITE bridge rejects compilation explicitly.

## Ownership and cancellation

`BlueprintValidationService` owns one lazily created worker, at most one active
request and one pending request, and one report. Each request owns its UTF-8 JSON
and copied runtime configuration; input is bounded to 1 MiB before the bridge's
worker copy. Submission replaces pending work, cancels active work and clears
old reports. Per-request stop states and a shared mutex protect publication, so
superseded results cannot become the latest report. Reports are delivered once;
an empty poll leaves the caller's output unchanged.

The core compiler accepts an optional `std::stop_token`. It checks cancellation
before parsing, between compilation stages and at every numerical probe sample.
Cancellation returns no graph. Destruction stops active work, discards pending
work, wakes and joins the worker before releasing its members. Compiled graphs
are destroyed on the worker. The worker owns no processor/runtime references
and executes no audio callback code.

The panel cancels validation when text changes, a draft reloads, the synthetic
example is inserted, a validating slot changes, or the panel is destroyed. The
bridge also rejects a pending report when host state or the observed audio
configuration changed before delivery. A delivered report describes its recorded
configuration; it is not a live CPU measurement or an audio installation.

## Review and verification boundaries

The compiler cancellation tests include pre-cancellation and interruption during
a maximum-rate numerical probe. The independent worker review records exact
source blobs in [the cancellation audit](../audits/R14_BLUEPRINT_CANCELLATION_REVIEW_2026-11-20.md).
The live bridge regression covers real successful/failed compilation, UTF-8 byte
admission, replacement/cancellation, one-time delivery, configuration/session
invalidation, unchanged host state/history, production-panel integration, LITE
rejection and worker destruction. Panel tracking tests cover draft and capability
behavior; they are explicitly synthetic UI fixtures.

The [R14 validation record](VALIDATION_2026-11-20_R14.md) records the completed
30-suite core and six-suite native passes, source identities and execution limits.
Source inspection and tracking fixtures alone do not qualify a native worker or
a host. Runtime graph swaps, complete sound-preset
transactions, dynamic session continuity, physical models and measured CPU
budgets remain separate work.
