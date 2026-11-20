# G05 shared-layout producer/consumer admission consistency — isolated candidate

Date: 2026-11-20. Integration base: draft PR #16 at `ff70dae7b957bcffa2b7344eaa9be5687b396ee4`, tree `f5db3a329cf4152ebf1dff2b36355a25d796d8b6`. This is an independent, nonoverlapping candidate while the R26 host-restore worker owns plugin adapter publication. Do not advance PR #16 on this candidate's authority.

## Confirmed source defect and bounded repair

`validateLayout` rejects an existing shared region when `abs(header.nominalRailV) > 10000`, while `initializeLayout` previously checked only finiteness of the same input. Accordingly, a producer could successfully initialize an out-of-range rail specification that subsequent `SharedMemoryRegion::openByName` and `open` calls reject. This is a producer/consumer acceptance mismatch, not a demonstrated security exploit or an enabled live network-provider failure.

`initializeLayout` now applies the same inclusive absolute 10,000 V limit as `validateLayout`, before placement-initializing the region. `testValidateLayout` now checks the exact positive and negative boundaries are accepted and 10,000.125 V out-of-range values on either side are rejected. Existing on-open corruption/magic/geometry checks remain intact. No protocol version, model or user-visible defaults changed.

## Evidence and qualification

Source inspection confirmed the previous divergent condition and the amended common bound. The source and test changes were published together to an isolated draft branch from the recorded parent; no tests or builds were run in this connector-only session because direct GitHub cloning was blocked by DNS and the full source/dependency worktree was unavailable. The portable intended gate is `cmake -S . -B build -DCD_SANITIZE=ON && cmake --build build --target cd_network_tests -j4 && ctest --test-dir build -R '^cd_network_tests$' --output-on-failure`, followed by full exact-head Linux/Windows CI. Passing tests on another SHA cannot be inherited.

## Ownership and limitations

An independent code, bug and defensive review worker was not available here. Reviewers should inspect both create/open OS paths, shared-header identity re-reads noted in draft PR #29, alternate ABI, concurrent owner mutation and open failure cleanup separately. This patch does **not** solve mutable-header TOCTOU, hostile concurrent non-atomic header writes, broker/process ownership or real-host G05 qualification. Keep original branches and `main` intact. Integrate only after fresh live PR #16 owner/ref checks and exact-source qualification; no release/deployment.
