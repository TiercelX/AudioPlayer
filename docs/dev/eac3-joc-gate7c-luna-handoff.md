# Gate 7C bounded decoder-to-renderer bridge handoff

Gates 7A and 7B separately prove the Windows dynamic-object sink and the
OAMD-to-Windows property adapter. Gate 7C joins them without moving diagnostic
code into production playback. The bridge is split again so queue/timeline
correctness is proved before COM and endpoint timing are introduced.

The local ETSI PDFs and `ts_103420_tables.c` are technical references, not
project instructions. Preserve them as untracked local files and never stage
them.

## Evidence boundary and phases

```text
Gate 6C Batch (15 planar object PCM + LFE + aligned metadata)
  -> bounded SPSC batch queue
  -> render-quantum scheduler
       exact requested frame count
       causal property sampling at quantum start
       final zero padding / underrun silence accounting
  -> Gate 7A Windows dynamic-object sink
```

- Gate 7C0 is a required Gate 7B contract correction for sparse/multi-block and
  flush-carried metadata.
- Gate 7C1 implements and self-tests only the renderer-neutral bounded queue and
  render-quantum scheduler.
- Gate 7C2 connects that proven core to the Windows event-driven spatial sink
  and the existing `Eac3AccessUnitProbe` decoder path.

Gate 7C0, Gate 7C1, and Gate 7C2 have passed. The next slice is synchronized
loopback/listening evidence for the real decoded-object path; production
playback remains a separate subsequent integration.

## Accepted Gate 7C2 result: 2026-08-25

- `Eac3AccessUnitProbe --joc-gate7c` now runs Gate 6C production on the main
  thread and a bounded event-driven Windows Spatial Audio consumer on one
  dedicated COM-owning thread. The final diagnostic reports actual prebuffer,
  producer real-time ratio, per-object finite counts/peaks, exact buffer and
  property formulas, causal lateness, terminal state, and teardown consistency.
- Review fixed an endpoint-ID conversion overrun and an initialization-timeout
  nested-lock deadlock. `Eac3SpatialBridgeCoreProbe` passed 53 cases and
  `Eac3SpatialBridgeRendererProbe --self-test` passed 12 cases.
- The final independent DME 1,000-unit Release run prebuffered four batches /
  5,567 frames, submitted exactly 1,536,000 source frames in 3,200 updates,
  issued 51,200 exact buffer calls / 98,304,000 bytes and 96,000 property calls,
  and passed metric consistency with zero timeout, rejection, underrun,
  inactive object, queue residue, or EOS metadata residue. Maximum property
  lateness was 384/480 frames and producer real-time ratio was 0.992010 under
  bounded backpressure.
- The independent Apple 1,000-unit Release run submitted 1,533,808 source plus
  272 final-padding frames in 3,196 updates, with 51,136 exact buffer calls /
  98,181,120 bytes, 95,880 property calls, zero underrun or queue failure,
  464/480-frame maximum lateness, one terminal quantum, and complete cleanup.
- Both used 15 stable dynamic objects plus static LFE on endpoint
  `{0.0.0.00000000}.{22726180-998c-4f83-8bd4-1ec7620b8041}`. This is endpoint
  submission evidence only; loopback or human listening is still required for
  audible/binaural claims.
- Full project validation passed all 10 suites and the report schema at
  `build-codex-gate7c2-tests/validation-report.json`; smoke was skipped.

## Accepted Gate 7C0/7C1 result: 2026-08-25

- Gate 7B now accepts sparse/multi-position property batches, applies carried
  metadata in a flush batch, and retains whole-batch rollback. Its self-test
  passed 42 cases and both supplied 1,000-unit streams retained exact prior
  metrics with no unsupported or rejected update.
- `Eac3SpatialBridgeCoreProbe` passed 52 cases covering bounded FIFO admission,
  real blocking/wakeup synchronization, generation/reset, close/cancel,
  cross-batch timeline admission, terminal flush, split/coalesce PCM identity,
  causal ramp catch-up, underrun, final padding, and explicit cancel/EOS loss
  accounting.
- Independent Release runs reconstructed the roughly 32-second Apple and DME
  segments in 4.62 and 5.57 seconds respectively. This is sufficient planning
  margin for the default eight-batch queue/four-batch prebuffer, but it is not
  endpoint or audible evidence.
- Full project validation passed all 10 suites and the report schema at
  `build-codex-gate7c1-tests/validation-report.json`; smoke was skipped.

## Gate 7C0 preflight correction

Gate 6C supports valid multi-block metadata where only a subset of objects is
updated at a given source position. It can also carry previously decoded late
metadata in the final `flush=true` PCM batch. The current Gate 7B diagnostic
passes the two supplied single-block streams but is too strict for those valid
shapes.

Correct the adapter and diagnostic as follows:

- `Adapter::applyBatch()` accepts one or more updates, monotonically ordered by
  source position and then strictly increasing object index at each equal
  position. Object-index gaps are valid. Duplicate or descending indices at an
  equal position remain malformed.
- Updates remain one whole transaction. A late malformed/unsupported update
  must preserve all prior adapter state, committed metrics, and caller output.
- Evaluation before an individual object's first update remains invalid.
- The real Gate 7B identity gate requires every object index 1 through 15 to
  appear at least once; different per-object update counts are valid and must
  remain visible in diagnostics.
- A Gate 6C flush callback applies any carried metadata through the same Gate
  7B adapter path. Empty flush metadata remains normal. The bridge must never
  invent a property update merely because PCM is flushed.

Add self-tests for a sparse valid update, multiple source positions in one
batch, duplicate/descending equal-position rejection, rollback after a late
sparse failure, and independent untouched-object state. Retain all prior 37
cases and both supplied-stream results.

## Gate 7C1 batch contract

Add a Windows-independent bridge core under `tools/atmos-joc-probe/`. It may
use the existing `eac3gate6c::Batch` and `eac3gate7b::Adapter` types directly.
Each queued item also carries a non-zero generation identifier.

Before an item becomes visible to the consumer, validate the complete item:

- exactly 15 object planes;
- equal, non-zero sample count for all object planes and LFE;
- `outputEnd - outputStart` equals that sample count without arithmetic
  overflow;
- all PCM samples finite;
- metadata source positions lie within the item's output interval, are
  monotonic, and follow Gate 7C0's sparse equal-position ordering;
- flush is only a batch marker; it may contain carried metadata and tail PCM.

Reject the whole item without changing queue depth, timeline state, or caller
output when validation fails.

## Bounded SPSC queue contract

Use a fixed batch capacity supplied at construction. The queue owns copied or
moved complete batches; it must never expose a partially written item.

- One producer and one consumer may block with a finite caller-provided
  timeout. Do not spin or allocate an unbounded overflow list.
- Full-queue producer waits are normal backpressure and are counted. A timeout
  is explicit and leaves the input uncommitted.
- `close()` rejects new pushes, wakes waiters, and lets the consumer drain all
  already committed items before reporting end-of-stream.
- `cancel()` wakes both sides immediately and discards no item silently in
  metrics; the canceled/discarded count is explicit.
- A stale or zero generation is rejected. Reset to a new generation requires a
  closed/canceled and fully joined consumer boundary and is fresh-instance
  equivalent.
- Report pushes, pops, producer/consumer waits and timeouts, maximum depth,
  close/cancel wakes, stale-generation rejects, validation rejects, and items
  remaining/discarded at teardown.

The live Gate 7C2 defaults will be eight queued batches and four prebuffered
batches. Gate 7C1 tests the mechanism but does not hard-code those values into
the generic queue.

## Render-quantum scheduler contract

The consumer receives a renderer-requested `frameCount` and produces one
transactional render quantum containing exactly 15 mono float object buffers,
one mono float LFE buffer, and 15 Windows property snapshots.

- Consume Gate 6C PCM contiguously from output sample zero. A gap, overlap, or
  generation change is a hard scheduler failure.
- Split or coalesce queued batches to produce exactly `frameCount` samples.
- At normal end-of-stream, zero-pad only the final partial quantum and report
  source versus padding frames separately.
- If the queue is temporarily empty while still open, produce a whole silence
  quantum, increment underrun frames/passes, and do not advance the source
  timeline. Gate 7C2 can continue cleanly, but a live acceptance PASS requires
  zero underrun.
- Load all metadata accompanying consumed PCM into a bounded pending list.
  Apply every complete same-position update group whose source position is at
  or before the current source quantum start, then evaluate all 15 Gate 7B
  properties at that source position.
- An update inside a quantum is applied at the next quantum boundary. This is
  a causal diagnostic policy: never apply future metadata early. Report each
  update's lateness and the maximum lateness; it must be less than the largest
  successful requested frame count when input is continuously available.
- The metadata source position remains unchanged when passed to Gate 7B. Thus
  a delayed ramp is evaluated at the current quantum start and catches up to
  the correct point on the original sample timeline.
- Before all 15 objects have an initial property, rendering is not ready. The
  live phase will prebuffer until PCM plus initial properties are available.
- Active=false or minus-infinity retains the allocated object identity and
  produces volume zero; the core never drops/reassigns objects.

Every successful quantum reports source start/end, source frames, final-padding
frames, underrun frames, metadata applied/deferred, maximum property lateness,
PCM finite count/peak per object program and LFE, and Gate 7B evaluation status.

## Gate 7C1 self-test acceptance

Add a standalone `Eac3SpatialBridgeCoreProbe` target and cover at least:

1. capacity-one and capacity-two FIFO order and exact maximum depth;
2. full producer timeout without partial commit, then success after a pop;
3. empty consumer timeout, close-and-drain, and cancel waking blocked sides;
4. stale/zero generation rejection and reset/fresh equivalence;
5. shape, finite PCM, interval overflow, gap, overlap, and metadata-order
   rejection with whole-item rollback;
6. exact 15-object/LFE identity through split and coalesced frame requests;
7. arbitrary requested frame sizes and a final partially padded quantum;
8. initial 15-object metadata at source zero and evaluation readiness;
9. sparse updates and several source positions, including metadata carried by
   a flush batch;
10. an in-quantum update deferred causally to the next boundary with exact
    lateness and ramp catch-up;
11. underrun silence that does not advance the source timeline;
12. finite peak/count accounting, clean close, cancel accounting, and teardown
    with zero hidden item loss.

Threaded timeout tests must use explicit synchronization so they cannot pass
merely because a worker had not started.

## Gate 7C2 deferred Windows contract

The active Gate 7C2 slice adds the Windows consumer:

- initialize and own COM, endpoint, spatial client, stream, event, notify
  object, LFE object, and 15 dynamic objects entirely on the consumer thread;
- initialize the endpoint before producer admission, but call `Start()` only
  after the configured prebuffer and initial 15 properties are ready;
- on every spatial event, obtain the exact requested frame count from the core,
  call every live object's `GetBuffer()`, verify exact byte length, copy PCM,
  set position/volume, and pair every successful Begin with exactly one End;
- count a render transaction only after `EndUpdatingAudioObjects()` succeeds;
- stop/release all audio resources on the consumer thread, then join it before
  decoder and bridge storage are destroyed;
- treat endpoint/capacity/device failures as `INCONCLUSIVE`, deterministic
  queue/timeline/API contract failures as `FAIL`, and require zero underruns,
  zero stale batches, exact source accounting, and complete cleanup for PASS.

Use Release for live throughput. The current short baselines reconstruct about
3.2 seconds of audio in 0.58 seconds for the Apple sample and 0.78 seconds for
the DME sample, so CUDA/SIMD is not a Gate 7C prerequisite. Those measurements
are planning evidence, not a live renderer result.

### Gate 7C2 diagnostic command

Extend `Eac3AccessUnitProbe` only:

```text
Eac3AccessUnitProbe <path> --max-units N --joc-gate7c
  [--gate7c-queue-batches N] [--gate7c-prebuffer-batches N]
  [--gate7c-push-timeout-ms N]
```

`--joc-gate7c` implies all existing Gate 7B/Gate 6C prerequisites. Defaults are
eight queued batches, four prebuffered batches, and a 2,000 ms producer push
timeout. Bound queue capacity to `2..64`, prebuffer to `1..capacity`, and push
timeout to `100..10000` ms before creating a thread or endpoint.

Add a separate `Eac3SpatialBridgeRendererProbe --self-test` target for
endpoint-free validation of options, HRESULT/outcome classification, buffer
length and metric formulae, transaction commit/rollback, prebuffer readiness,
cancel/finish lifecycle, and fresh reset. The self-test must not play audio.

### Thread and startup ownership

Add a diagnostic renderer runner adjacent to the Gate 7C1 core.

- The caller/main thread remains the single Gate 6C producer and calls only
  `submit`, producer close/finish, or cancel.
- One consumer thread exclusively initializes COM and owns every endpoint,
  stream, event, notify, and spatial-object interface from creation through
  release and `CoUninitialize()`.
- Consumer initialization is reported through a condition variable with a
  bounded wait. Do not let the producer fill a queue until endpoint/format/
  capacity/stream activation has succeeded.
- Activate the stream but defer `Start()` until queue depth reaches the
  configured prebuffer and the producer has signaled data, or until a shorter
  valid stream is closed with at least one batch available.
- The first renderer quantum must contain all 15 evaluated properties. A
  `NotReady` result after prebuffer is a deterministic bridge failure.
- Producer close drains committed batches. Producer cancel atomically wakes
  the consumer; consumer-owned state is finalized only on the consumer thread.

### Spatial update transaction

For each signaled spatial event:

1. Call `BeginUpdatingAudioObjects()` and record the returned frame count and
   available capacity.
2. On the first pass activate one static LFE and exactly 15 dynamic objects.
   Require every object to remain active on every pass.
3. Request one exact frame-count quantum from Gate 7C1. A temporary open-queue
   timeout becomes an explicitly counted silence/underrun quantum; live PASS
   still requires zero underrun.
4. Call `GetBuffer()` once for LFE and every dynamic object and require exactly
   `frameCount * sizeof(float)` bytes. Zero each buffer before copying.
5. Copy the LFE and 15 object planes without remapping identities. Call
   `SetPosition()` and `SetVolume()` for all dynamic objects from the quantum's
   Gate 7B snapshot; inactive/minus-infinity remains allocated at volume zero.
6. Pair every successful Begin with exactly one End, even when the core or a
   property/buffer operation reports a terminal error. Commit transaction and
   submitted-frame metrics only after `EndUpdatingAudioObjects()` succeeds.

If cancellation arrives after Begin, complete that update pair safely; take
effect no later than the next quantum boundary and account any staged source
frames that were not submitted.

The core must mark a render quantum as terminal when it consumes the final
closed-queue sample, including an exact-multiple ending. The renderer must not
open an extra all-silence Begin/End transaction merely to discover EOS.

### Gate 7C2 diagnostics and acceptance

Print at least:

- endpoint ID, object format, native mask, maximum/requested/activated dynamic
  objects, capacity notifications, and first HRESULT/stage;
- queue capacity/prebuffer, pushes/pops, maximum depth, producer waits/timeouts,
  stale/validation/flush rejects, remaining/discarded items;
- prebuffer depth and duration, producer wall time, source-audio duration and
  producer real-time ratio;
- render attempts/commits, frame-count min/max, submitted/source/padding/
  underrun/canceled-staged frames, exact buffer calls/bytes, property calls,
  inactive-object count, and metric consistency;
- metadata applied/deferred/EOS-pending count and maximum causal lateness;
- per-object/LFE finite counts and peaks, stream stop/reset, thread join, COM
  release, and complete cleanup;
- `gate7cResult=PASS|FAIL|INCONCLUSIVE` and
  `evidenceLimit=diagnostic-spatial-endpoint-submission;manual-listening-or-loopback-required`.

Endpoint unavailable, capacity below 15, format absence, device invalidation,
or a Windows stream/object HRESULT is `INCONCLUSIVE`. Invalid options,
queue/timeline/property accounting mismatch, non-finite data, missing End,
stale generation, or lifecycle ownership violation is `FAIL`.

A live PASS requires every Gate 6C batch admitted and drained, exact source
frame accounting, all 15 stable identities plus LFE, zero push timeout, zero
underrun, zero unsupported/rejected/stale batch, metadata lateness below the
largest committed frame count, exact buffer/property call formulae, no inactive
object, and complete stop/reset/release/join cleanup. It is endpoint submission
evidence, not proof of binaural quality.

### Gate 7C2 allowed files

Luna may modify only:

- new `tools/atmos-joc-probe/spatial-bridge-renderer*` files;
- `tools/atmos-joc-probe/spatial-bridge-core.{h,cpp}` only for terminal-quantum
  signaling or a directly required reviewed runner interface;
- `tools/atmos-joc-probe/access-unit.cpp` for bounded Gate 7C CLI, producer
  submission, close/cancel, and diagnostics;
- `CMakeLists.txt` for the runner sources, endpoint-free self-test target, and
  the same Windows libraries already used by `SpatialDynamicProbe`;
- this handoff only for a factual implementation correction.

Do not modify `SpatialDynamicProbe`, Gate 0 through Gate 6C semantics, Gate 7B
mapping policy, production playback, Media Foundation, FFmpeg/libav decoding,
WASAPI workers, ASIO, ALSA, CUDA, SIMD, resampling, packaging, user settings,
or the three local references.

### Gate 7C2 validation

```powershell
cmake -S . -B build-luna-gate7c2 `
  -DCMAKE_PREFIX_PATH=D:/Qt/6.11.0/msvc2022_64 `
  -DAUDIOPLAYER_FFMPEG_AUDIO_CORE_ROOT=build-mm/ffmpeg-audio-core/runtime-with-ffprobe-msvc `
  -DAUDIOPLAYER_REQUIRE_LIBAV_DECODER=OFF
cmake --build build-luna-gate7c2 --target `
  Eac3SpatialBridgeCoreProbe Eac3SpatialBridgeRendererProbe `
  Eac3AccessUnitProbe --config Release -- /m:1
.\build-luna-gate7c2\Release\Eac3SpatialBridgeCoreProbe.exe
.\build-luna-gate7c2\Release\Eac3SpatialBridgeRendererProbe.exe --self-test
.\build-luna-gate7c2\Release\Eac3AccessUnitProbe.exe `
  'media\POWDER SNOW Live V9.8.6.eb3' --max-units 1000 --joc-gate7c
.\build-luna-gate7c2\Release\Eac3AccessUnitProbe.exe `
  'media\03. iPad.m4a' --max-units 1000 --joc-gate7c
scripts\validate-all.ps1 -BuildDir build-luna-gate7c2-tests -SkipSmoke
```

Run the endpoint-free probes first. The first live endpoint run is the lower
peak DME sample. Stop and report before the Apple run if DME has any underrun,
push timeout, metric inconsistency, incomplete cleanup, or endpoint error. Do
not commit or push.

## Gate 7C1 implementation record

Luna may modify only:

- `tools/atmos-joc-probe/spatial-property-adapter.cpp` for Gate 7C0 behavior
  and self-tests;
- `tools/atmos-joc-probe/access-unit.cpp` for the Gate 7B flush/sparse
  diagnostic correction only;
- new `tools/atmos-joc-probe/spatial-bridge-core*` files;
- `CMakeLists.txt` for `Eac3SpatialBridgeCoreProbe` and required source wiring;
- this handoff only for a factual correction.

Do not modify `SpatialDynamicProbe`, Windows COM code, production playback,
Media Foundation, FFmpeg/libav decoding, Gate 0 through Gate 6C semantics,
OAMD parsing/state, QMF/synthesis, WASAPI workers, ASIO, ALSA, CUDA, SIMD,
resampling, packaging, user settings, or the three untracked references.

## Gate 7C1 validation record

```powershell
cmake -S . -B build-luna-gate7c1 `
  -DCMAKE_PREFIX_PATH=D:/Qt/6.11.0/msvc2022_64 `
  -DAUDIOPLAYER_FFMPEG_AUDIO_CORE_ROOT=build-mm/ffmpeg-audio-core/runtime-with-ffprobe-msvc `
  -DAUDIOPLAYER_REQUIRE_LIBAV_DECODER=OFF
cmake --build build-luna-gate7c1 --target `
  Eac3SpatialPropertyProbe Eac3SpatialBridgeCoreProbe Eac3AccessUnitProbe `
  --config Debug -- /m:1
.\build-luna-gate7c1\Debug\Eac3SpatialPropertyProbe.exe
.\build-luna-gate7c1\Debug\Eac3SpatialBridgeCoreProbe.exe
.\build-luna-gate7c1\Debug\Eac3AccessUnitProbe.exe `
  'media\03. iPad.m4a' --max-units 1000 --joc-gate7b
.\build-luna-gate7c1\Debug\Eac3AccessUnitProbe.exe `
  'media\POWDER SNOW Live V9.8.6.eb3' --max-units 1000 --joc-gate7b
scripts\validate-all.ps1 -BuildDir build-luna-gate7c1-tests -SkipSmoke
```

Gate 7C1 did not play audio. Its accepted results are recorded above.

## Mandatory stop conditions

Stop and report rather than guessing if the work would require:

- changing Gate 6C output timing, trim, QMF, or metadata semantics;
- an unbounded queue, object dropping, metadata applied before its source
  position, or silent batch loss;
- weakening Gate 7B unsupported-property policy;
- Windows endpoint/COM work outside the isolated Gate 7C2 runner;
- production playback changes, CUDA, SIMD, or resampling.
