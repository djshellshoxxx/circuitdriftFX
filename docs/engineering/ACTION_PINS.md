# GitHub Actions source pins

Resolved directly from each official repository's `refs/tags/v4` API endpoint on
2026-11-19. Both CI and release workflows now use these immutable commit IDs. This
does not change their required jobs, validation thresholds, permissions or triggers.
Checkout does not retain repository credentials in the job's working tree.

| Action | Commit | Verified source |
| --- | --- | --- |
| checkout | `11d5960a326750d5838078e36cf38b85af677262` | <https://api.github.com/repos/actions/checkout/git/ref/tags/v4> |
| cache | `0057852bfaa89a56745cba8c7296529d2fc39830` | <https://api.github.com/repos/actions/cache/git/ref/tags/v4> |
| upload-artifact | `ea165f8d65b6e75b540449e92b4886f43607fa02` | <https://api.github.com/repos/actions/upload-artifact/git/ref/tags/v4> |
| download-artifact | `d3f86a106a0bac45b974a628896c90dbdf5c8093` | <https://api.github.com/repos/actions/download-artifact/git/ref/tags/v4> |

Future updates must resolve and review the new upstream source, then change the pin
deliberately. The `v4` comments describe the tag resolved at this checkpoint, not a
moving execution reference.
