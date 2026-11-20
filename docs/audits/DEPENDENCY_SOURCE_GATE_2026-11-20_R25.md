# Dependency verification before configuration — R25 and R26

## Finding and repair

R24 source review at b84d2ba found that FetchContent_MakeAvailable configured
upstream CMake before cd_verify_pin, and that the verifier checked only HEAD.
An edited or incorrectly populated checkout could escape the claimed source
check or be rejected after its configuration code ran. This is a verification
boundary defect; no compromised source or external cache poisoning was demonstrated.

Fresh R25 commits ed0e055 and c2f8a88 integrated as 6ae78a5 and 51029e0.
[PinnedDependencies.cmake](../../cmake/PinnedDependencies.cmake) separates
acquisition from configuration. All four expected revisions and clean worktrees
are checked before either JUCE or clap-juce-extensions is added. Verification
includes directory identity, normal index flags, and tracked, staged, untracked
and ignored contents. Existing checkouts are never reset or repaired. Explicit
source overrides remain read-only; missing disconnected sources fail without fetching.

The fully specified FetchContent_Populate form acquires only missing sources,
avoiding the deprecated single-name form and CMP0169 OLD. Population subbuilds,
stamps and working files stay under the build directory. The second commit
shadows legacy FETCHCONTENT_BASE_DIR values locally: newer policy behavior
otherwise created temporary files inside the source cache. SHA pins are unchanged.

[CI](../../.github/workflows/ci.yml) caches only juce-src and
clap_juce_extensions-src under a new key covering both CMake files.
[Release](../../.github/workflows/release.yml) acquires fresh sources without
restoring that cache. Validator changes have their own
[repair record](RELEASE_VALIDATOR_GATE_2026-11-20_R25.md).

## Executed evidence

| Source / scope | Result |
| --- | --- |
| Worker final c2f8a88 | 24 owned compiler-free CMake/Git cases passed in 3.848 s; CMake 4.4.5, Git 2.47.3, Python 3.12.14. |
| Legacy-cache test against ed0e055 | Failed as intended: juce-tmp and clap_juce_extensions-tmp appeared in the source cache. The identical case passes after c2f8a88. |
| Real cached source verifier | All four pinned checkouts accepted using Git 2.56.1; no upstream CMake executed by that worker check. |
| Root integrated 51029e0 | Real native JUCE/CLAP project reconfigured, including juceaide setup and both Full/LITE CLAP targets. |
| Root CTest at 51029e0 | dependency_source_gate passed 1/1 in 4.48 s (case 4.46922 s), executing all 24 owned cases through the real build registration. |
| Root incremental six-target build | Completed with no work; runtime/plugin/native-test C++ inputs remain unchanged from the qualified GIF source. |

Cases cover clean overrides/cache/fresh local acquisition, new CMake policies,
root/submodule pin mismatches, directory identity, tracked/staged/untracked/ignored
edits, index flags, missing metadata/submodules, disconnected/invalid overrides and
legacy cache placement. Every negative case checks that neither dependency
configuration marker ran. Fixtures are small repository-owned projects.

Evidence: [worker output](../evidence/dependency-gate-2026-11-20-r25.log),
[prior failure](../evidence/dependency-gate-prior-cache-2026-11-20-r25.log),
[real sources](../evidence/dependency-gate-real-verify-2026-11-20-r25.log),
[native configuration](../evidence/dependency-gate-native-configure-2026-11-20-r26.log),
[integrated CTest](../evidence/dependency-gate-ctest-2026-11-20-r26.log),
[JUnit](../evidence/dependency-gate-ctest-2026-11-20-r26.xml), and
[incremental build](../evidence/dependency-gate-incremental-2026-11-20-r26.log).

## Fresh review and limits

All R25 workers completed before R26 successors began. Fresh code and defensive
review of 51029e0/tree57c2368dc5cb4e2bd8540c54a2ab3dbd7a0b5431 found no new
actionable finding in their assigned scopes. Both used immutable source. The code
reviewer also parsed YAML and Python and checked workflow calls, timeouts, caches,
16 PowerShell cases, 24 dependency cases and CTest registration. Neither reviewer
executed PowerShell, dependency fixtures, upstream CMake or project builds.

This boundary trusts local Git/toolchain metadata and assumes sources are not
modified concurrently with configuration. It is not an upstream vulnerability
assessment or whole-machine integrity check. Helpers target portable CMake 3.22;
plugin configuration separately requires 3.25. Only 4.4.5 was executed locally.
Windows execution of the 16-case PowerShell harness and real strictness-5 validation
of both products remain required at the published source. No release occurred.
