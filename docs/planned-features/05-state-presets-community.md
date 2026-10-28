# State, A/B, undo and community sharing

Planned contract: CD-F04/F09/F24; T04/T09/T24. Related: blueprints, runtime, [UX](08-ux-interaction-and-displays.md). Sources S01/S02/S07/S13. Offline/local operation never requires a cloud login.

## State ownership and A/B

One session contains global macros/utilities/quality, slot A, slot B and UI preferences. Each slot contains blueprint revision, base parameters, timeline/fault configuration and component/random identities. Both full-edition engines run continuously on identical input/sidechain so switching includes genuine thermal/electrical history. They use independent state and declared seeds. Global macros apply to both; A/B compares the selected circuits under the same macro settings. GUI must say this rather than imply comparison of global output trim or quality.

A/B comparison button changes crossfade endpoint with 20 ms equal-sum linear fade. Continuous crossfade 0..1 is also available. Identical slots must remain unity at all positions; equal-power mixing is inappropriate for correlated identical signals. Latency is equal across both slots. Copy A->B copies configuration and a versioned dynamic-state snapshot, including seeds, then starts B from that snapshot at a sample boundary. Swap swaps slot identities/configurations/dynamic states atomically while complementing mix to keep the sound unchanged. Each operation is one undo transaction.

Persistent project state includes configuration plus optional exact engine checkpoint (version/model/Fs-qualified), sample-clock remainder, electrical/thermal/dielectric/magnetic state, delay lines and RNG states. Capture by preallocated snapshot request at a block boundary, then serialize on a worker; no APVTS copyState/replaceState on the callback (S13). A host save requiring immediate data gets the last coherent published checkpoint with its sample index; no waiting for an audio thread that may be stopped. Configuration persistence is mandatory even if dynamic checkpoint is unavailable. On compatible reload restore checkpoint; incompatible sample rate/model resets dynamic state to prepared warm initialization and reports that fact. Avoid claiming exact continuity when only configuration is restored.

## JSON presets and validation

Extension: .circuitdrift.json, UTF-8 JSON, schemaVersion=1. Envelope fields: product="CircuitDrift", kind="scene"|"session", engineModelVersion, minReaderVersion, author/title/tags, createdAt, requiredFeatures, blueprint(s), baseParameters, macros, timeline, faults, seed and optional UI preferences. Publication strips private UI paths, session/network tokens, checkpoints, account credentials, file paths and any captured audio.

Scene is one circuit plus authored macro snapshot. Default Load Scene loads selected slot and its macro snapshot as one visible transaction, showing that global macros affect both slots. "Load circuit only" preserves current macros and explicitly warns that the authored sound may differ. Session loads both slots/global configuration. Export scene does not include accidental dynamic heat/RNG history; "Export checkpoint" is a separate local debugging/session action, not community content.

Bounds: scene <=1.25 MiB and session <=2.5 MiB uncompressed, no archives or embedded executable/media payloads, JSON depth <=20. Blueprint-specific 1 MiB bound still applies. Reject duplicate keys, non-finite numbers, unknown required feature/model, incompatible major version and out-of-range values; do not silently clamp hostile/unsupported presets into a different sound. Harmless unknown metadata fields may be ignored. Migrations are pure ordered vN->vN+1 transformations on a worker; preserve unknown metadata only within bounded size. Keep migration fixtures and result digests. Atomic save writes a temporary file then replaces on successful flush/close outside audio.

## Twenty-step undo/redo

A completed mouse drag, wheel gesture (250 ms idle grouping), numeric edit, Apply graph, fault Apply, timeline scrub, scene load, A/B copy/swap or macro change is one transaction. Each transaction holds previous/next configuration and the affected IDs, label and origin. Retain at least 20 completed transactions plus current state; redo is cleared by a new edit. Store immutable shared graph blobs and diffs; hard history budget 56 MiB covers 21 worst-case 2.5 MiB sessions plus metadata. Do not allocate a full audio delay/checkpoint per undo step.

Undo changes configuration through normal ramps and prepared graph transitions; it does not rewind the physics clock. Host-driven automation, incoming meter values, temperature evolution, transport, cloud refresh and render frames are excluded. UI undo uses normal host begin/perform/end gestures. The host remains authoritative: subsequent automation can override the restored setting; show Automated rather than fighting the DAW. During active automation writing, do not inject old recorded automation into the host's history.

Read/write history on the message thread; publish coherent transactions to audio. Undo cannot reverse external publish/delete, local file save, recording or account changes. Those operations do not occupy the parameter history. Keep history per instance, transient across project restart by default. A project load replaces state and clears undo. T09 must undo/redo exactly 20 worst-case supported transactions and restore canonical configuration equality.

## Community browser and API

Guest browse/download; login required for publish, edit, delete, report and account favorites. Browse panels: Factory, Local, Community, Favorites; filters type/topology/author/tag/required edition/version; paginated search, 50 items/page, cursor token. Selecting a result shows compatibility before Apply. No automatic audible preset loading on hover or background update.

Proposed HTTPS JSON API v1:
| Method/path | Request | Result |
|---|---|---|
| GET /v1/presets | q, cursor, limit<=50, tags, engineVersion | items, nextCursor, ETag |
| GET /v1/presets/{id}/revisions/{rev} | immutable identifiers | metadata, blobSha256, byteLength, download URL |
| POST /v1/presets | title, description, tags, license, validated scene JSON, idempotencyKey | private draft ID/revision |
| POST /v1/presets/{id}/publish | expectedRevision, visibility=public | published immutable revision |
| PATCH /v1/presets/{id} | If-Match revision, metadata/new scene | new private revision |
| DELETE /v1/presets/{id} | authenticated owner + expectedRevision | tombstone, cached copies remain local |
| POST /v1/reports | presetId, revision, reason enum, optional text | report ID |

All responses have requestId and schemaVersion; errors contain code/message/retryAfter. Use 400 invalid, 401 auth, 403 ownership, 404 absent, 409 version conflict, 413 size, 429 rate limit and 503 unavailable. Idempotency keys persist per attempt so a retry cannot duplicate publication. Owner checks and blueprint validation occur server-side as well as client-side. Immutable content hash is SHA-256 over exact downloaded bytes; validate digest before parse.

Server stores metadata/revisions in a transactional database and scene blobs in object storage; public read path/CDN cannot mutate revisions. New uploads enter quarantine, pass resource/schema/model/content checks and moderation before publication. Disallow arbitrary external media/script URLs. User-entered markup is displayed as text. Reports/takedown make a revision unavailable for new download but must not silently break an already saved DAW project. Legal ownership/license choice is part of publication UX, not guessed from a public URL.

Authentication: native public-client OAuth authorization code with PKCE in system browser, state/nonce and registered loopback/custom callback per platform (S07). No embedded login/password harvesting or shipped client secret. Store refresh tokens in OS credential storage through a worker; tokens never enter presets, project state, logs or IPC. Backend/provider selection and OAuth application registration remain release blockers, documented separately.

Network limits: at most 2 requests concurrent, 10 s connect/30 s total timeout, cancellation on panel close, max response sizes above, retry reads at 1/2/4 s plus bounded jitter (three retries), honor Retry-After. Publish requires an explicit final click; no automatic re-publication. Transport and failures cannot affect audio. Local cache 250 MiB with LRU for unpinned downloaded files, maintained on worker. Imported project presets are embedded in project state, independent of cache/cloud availability.

## Acceptance and budgets

T04 uses a mock service to test offline browse/cache, timeout, cancellation, expired auth, forged ownership, malicious JSON, digest mismatch, 409/429, duplicate retries and removal of a published item. Verify zero network activity until Community or login is explicitly used. T24 tests identical-slot unity, 20 ms continuity, coherent copy/swap, compatible checkpoints and all migrations. History+engine+GUI must still fit the full 120 MB idle instance target; allocate runtime factory buffers <=32 MiB, history <=56 MiB and GUI/other <=20 MiB as an initial partition and benchmark actual shared/runtime overhead. Large capture buffers are separately accounted in exporter mode.
