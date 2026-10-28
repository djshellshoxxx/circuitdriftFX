# Multi-instance shared PSU and cross-talk

Planned contract for CD-F11; test T11. This feature is a major scheduling/integration risk, not ordinary shared-variable communication. A plugin cannot assume a host runs every track at the same time, in the same process, or at realtime speed (S01/S03). Full deterministic offline coupling needs explicit replay/co-render support.

## Scope and topology

Join is off by default. A user creates a local group with random 128-bit group UUID, human name and explicit session key; membership is opt-in per instance. Never merge instances by preset name or globally shared static variables. Max 32 members/group, 8 groups per local broker. No Internet traffic is involved. Cross-project/session isolation is by user-created group and ephemeral session token because portable formats do not guarantee a unique project ID.

Use a per-user broker process for discovery/control and OS shared memory for prepared data rings. Windows/macOS adapters must constrain object permissions to that user, include protocol version, and handle plugin sandbox restrictions. Shared-memory setup, connection and broker startup occur off audio. Audio maps only preopened memory, no system calls or locks. Atomics used across processes require a platform-validated layout/ABI and lock-free implementation; do not assume every std::atomic is an IPC primitive.

Protocol envelope: magic "CDIP", major/minor version, byte size, group/session/member UUIDs, generation, sample rate, channels, timeline epoch, absolute sample start, sequence, payload type and payload length. Reject incompatible major, mismatched Fs/epoch, invalid size or stale generation before consumption. Payload memory is POD with no pointers. Producer publishes a fully written slot with release; consumer acquires once and reads only its owned slot. Preallocate SPSC rings per member/direction. No spin-until-ready or seqlock retry loop on audio.

## Two independent coupling paths

**Shared demand/PSU:** Each member contributes supply current demand [A], power [W], rail model identity and normalized activity over fixed 128-frame quanta. Broker owns one shared reservoir model for the group. Members must use the same compatible PSU manifest, nominal rail and sample rate; different tube/transistor rail systems cannot be aggregated by pretending volts are interchangeable. Incompatible membership is rejected with a per-member reason.

**Audio bleed:** Opt-in separately, level default -90 dB, range Off or -90..-24 dB. Tap member audio after input trim and before its circuit/received cross-talk to prevent recursive network feedback. For receiver i, c_i=g/(max(1,N-1))*sum(j!=i) p_j*x_j, where p_j is a fixed seeded sign and g=10^(bleed_dB/20). Optional topology matrix must have absolute row sum <=g. Default is same-channel stereo bleed, mono duplicates at -3 dB into stereo. Scope explicitly models pre-circuit console coupling; it does not claim all hardware cross-talk paths.

## Realtime scheduling and deterministic limits

A quantum is 128 samples. For group tick k, consume a completed broker result for k-L only, never a partly updated peer buffer. Proposed live L=8 quanta (1024 samples). Network delay on the audio-bleed path is a sonic cross-talk delay, not automatically compensated host output latency; main-path samples are not delayed merely to hide it. Display 1024/Fs seconds. Shared demand is also delayed and therefore labeled approximate live coupling.

Broker advances a tick only when it has all active-member frames for it, or a 50 ms realtime deadline expires. Missing inputs then contribute zero new audio and last current demand decayed with 100 ms tau. Mark degraded membership. Audio consumer finding no exact frame uses the same decay/zero rule locally, never waits. Ramp joining/leaving influence 50 ms. After 500 ms no heartbeat, remove member via worker and publish a new roster generation. Broker restart fades to the independently running local PSU over 50 ms; it never stops audio.

Do not call this protocol sample-perfect under arbitrary DAW scheduling. If host prefetch/offline timing, transport epoch, or sample rate invalidates realtime group synchronization, publish Network unavailable status and fade to local PSU with zero bleed. This fallback is explicit in UI and output metadata; no silent assertion of a coupled bounce.

**Deterministic render mode:** Planned companion/co-render harness processes all participating dry stems and automation against a common clock. It executes all member demands and one PSU solve in fixed member-ID order per quantum, records the group rail plus peer bleed streams in a versioned influence bundle, and replays those exact streams on subsequent plugin renders. Manifest includes sample rate/start/length, group topology, model versions, seeds and per-input content/automation hashes. Any hash/routing/start mismatch invalidates replay. For nonlinear demand depending on rail, solve all members and shared rail jointly in the harness or use documented fixed-point iteration bounded to 8 iterations; reject unconverged renders. This is necessary work for reliable offline equivalence, not an optional promise that IPC alone solves host ordering.

## Performance and test contract

Each mono/stereo member gets fixed 64-quantum rings; audio callback performs bounded copies and one ready-slot read per quantum. Target network overhead <=0.05 ms p99/member at 48 kHz/128 on the runtime reference machines; group broker budget <=1 ms/tick for 32 factory members. Those are unmeasured targets; exact coupled co-render may be slower than realtime.

T11 includes two and 32 instances in one and separate processes, incompatible Fs, project-group isolation, reorder/delay/drop of peer callbacks, GUI closure, broker death, seek/loop, suspend, prefetch and offline. Require no audio waits/allocations, no invalid samples, correct status/fallback and bounded cross-talk gain. Deterministic replay must reproduce its reference bundle within 1e-6 peak error, independent of host callback order/block partition. Live mode is tested for bounded error and documented delay, not falsely for bit-identical nondeterministic scheduling.
