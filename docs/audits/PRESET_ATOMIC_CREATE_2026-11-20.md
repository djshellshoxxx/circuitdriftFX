# Atomic preset no-overwrite creation follow-up — 2026-11-20 UTC

## Finding and owner-visible behavior

PR #19 introduced a production preset library with a no-overwrite save/import mode. The initial `PresetLibrary::save` ran a `symlink_status/exists` test and then called `saveStateFileAtomic`, whose final `rename()` (POSIX) or `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING)` (Windows) could overwrite any destination that appeared after the check. This breaks the promised no-overwrite behavior for concurrent local writers and can replace another writer's preset.

## Isolated repair

On `codex/cdfx-preset-atomic-create-20261120`, based on draft PR #19 commit `e9c00c51cdf155127cd839c446126ae1d8f9f6c6`:

- Preserve the existing replace-mode API and its durable-temp-file write/flush/close behavior.
- Add `saveStateFileAtomicNoReplace` with an exclusive destination-install step. On POSIX, after flushing/closing the temporary file, `link(temporary, destination)` creates only when the destination does not exist; then the temporary name is unlinked. On Windows, the same exclusive temporary-file creation and flush are retained, but `MoveFileExW` is called with `MOVEFILE_WRITE_THROUGH` and without `MOVEFILE_REPLACE_EXISTING`.
- Route local user preset `overwrite=false` through the exclusive installer. A pre-existing non-overwrite check remains for a friendly failure message but is no longer the correctness condition.
- Test both already-existing target preservation and two concurrent writers to a previously absent target. Exactly one writer must succeed and its content must survive; temporary files must not remain. Existing overwrite-mode tests remain.

## Review scope and limitations

The destination directory remains a trusted user-local directory; this change is **not** an openat/no-follow directory-descriptor confinement API, nor is it intended to handle hostile directory replacement by a privileged adversary. Concurrent rename of the *parent directory*, filesystem-specific hard-link restrictions, Windows antivirus/locking interference and power-loss directory durability are not established here. The exclusive install prevents a race between ordinary local writers, including against an existing dangling symlink.

Source changes were checked for exact intended replacements and the machine-readable consolidation-state JSON parsed successfully. Native compilation, targeted state_preset execution, sanitizers, Windows plugin packaging/pluginval, real-host runs and independent reviewer signoff have **not** been executed in this connector-only run; exact-head CI is mandatory and must be attributed to the new source, not PR #19.

## Next ownership

1. Verify Linux `state_preset` (especially concurrent-create regression) and Windows exact-head CI/pluginval after opening the draft child PR.
2. Fresh independent code reviewer: examine preservation of overwrite behavior, error handling, temporary-file lifecycle and ABI.
3. Fresh independent bug hunter: test competing threads/processes, dangling symlink destination and Unicode filename behavior on Windows.
4. Fresh defensive security reviewer: inspect trust assumptions for directory/paths, symlink and reparse points, hostile competing writers and failure modes.
5. Once qualified, confirm live coordinator/parent heads and integrate by an authorized, guarded, non-forced update; do not overwrite other workers' changes. Keep full G12 persistence and live sound preset transactions separate.

All models remain synthetic; no release, deployment or branch deletion.
