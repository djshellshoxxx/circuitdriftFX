# G05 in-process and mapped group admission parity — isolated candidate

Date: 2026-11-20. Source parent: PR #30 at `5887bda1a4f7e4d99d296acb8f9eec4fd2d1f434`, itself based on PR #16 `ff70dae7b957bcffa2b7344eaa9be5687b396ee4`. This change is separate from the active host-state R28 root. It does not authorize advancing PR #16.

## Verified source defect

Mapped `validateLayout` and (after PR #30) `initializeLayout` reject nominal rail magnitude above 10,000 V, but `Group::join` accepted finite out-of-range rails and `Registry::join` checked only group identity and channel count before constructing an in-process header. A fresh in-process group could thus be admitted with a header that the mapped transport rejects. In addition, invalid sample rates reached registry allocation/eviction before the more complete join check; an invalid request could receive `GroupLimit` at capacity rather than `InvalidArgument` and could discard an otherwise recyclable empty group.

## Repair

A single `kMaxNominalRailV` constant governs mapped create/open and in-process joining. `Group::join` now rejects out-of-range rails. `Registry::join` preflights sample-rate and rail finiteness/ranges, plus existing group/channel identity, before allocating or recycling any group. No wire layout, header offsets, or runtime audio processing is changed.

A new `testGroupSpecAdmission` regression exercises NaN, infinity, out-of-range rates and rails, invalid channels, acceptance of both exact rail boundaries, rejection without retained groups, a successful valid join after rejection, and direct `Group::join` range enforcement.

## Evidence, owners and gates

**Source-only verification at publication**: exact source parent, checked unique edit sites, added concrete regression assertions; compilation and test execution were **not** available in this connector session (GitHub clone DNS unavailable). Required: sanitized `cd_network_tests`, exact-head Linux/Windows CI, independent code/bug/defensive review, and integration owner approval. CI on PR #30 does not qualify this child.

This does **not** resolve shared-memory mutable-header TOCTOU, concurrent hostile peers, ABI assumptions, provider activation, real-host G05 qualification or broader G12 checkpoint work. Preserve the R28 host-state integration, PR #16, `main` and all original branches.
