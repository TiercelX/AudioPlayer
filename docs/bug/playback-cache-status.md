# Playback cache status

This file tracks playback source preparation, sidecar remux cache behavior,
cache settings UI, pruning, and local diagnostic/cache retention issues. Keep
durable workflow rules in `AGENTS.md` and `docs/dev/*.md`.

## Durable guidance

- Bug/status tracking index: `docs/bug/README.md`
- Workflow and change scope: `docs/dev/agent-workflow.md`
- Harness and smoke-test policy: `docs/dev/harness.md`
- Diagnostics and evidence layers: `docs/dev/diagnostics.md`
- Git and release workflow: `docs/dev/release-workflow.md`

## Status refresh: 2026-06-13

- **ALSA PcmSeekCache initialization**: The ALSA backend now lazily initializes
  `PcmSeekCache` in `startPipeline()` when libav seek decoder is available,
  matching the WASAPI pattern. Dolby content (AC3/EAC3/TrueHD/raw streams) gets
  64 MiB default when `maxPcmCacheMiB` is not user-configured. Cache is passed
  to `LibavSeekDecoderWorker` via `setSeekCache()`, cleaned up on source switch
  in `setSource()`, and deleted in the destructor. Build: WSL `cmake --build
  build-linux` — 0 errors, 0 warnings.
- **Files changed**: `src/backends/alsa/linuxalsaaudioplayer.cpp`,
  `src/backends/alsa/linuxalsaaudioplayer_state.cpp`.
- **Validation**: Linux WSL build only; no runtime smoke test in this pass.

## Status refresh: 2026-06-06

- **Raw Dolby seek fix**: Raw Dolby sidecar remux now writes Matroska with
  `-cluster_time_limit 100` and a new sidecar cache key version. The old sidecar
  seek to 30s landed on a 25.061s packet/key point; the tight-cluster sidecar
  lands at 29.968s for the same target, avoiding several seconds of decoder
  discard work after seek.
- **PCM seek-cache safety**: Restored a targeted PCM seek cache path for raw
  Dolby streams without reintroducing the large in-memory compaction risk.
  `PcmSeekCache` memory mode now stores independent segment blocks and prunes by
  dropping oldest segments instead of moving a single large buffer. This keeps
  decoder-thread cache writes bounded when the cache reaches its limit.
- **Raw Dolby behavior**: When the PCM cache size has never been explicitly
  saved, WASAPI libav playback now enables a small 64 MiB seek cache for raw
  Dolby streams, including sources that were first remuxed to a Matroska
  sidecar for playback. A user-saved value of `0` still means explicitly
  disabled.
- **PCM seek-cache hit quality**: PCM seek-cache writes now coalesce adjacent
  tiny decoder writes into bounded segments of up to about 1 second. This keeps
  pruning cheap by dropping segment blocks while avoiding zero-duration,
  few-hundred-byte cache hits that barely help a repeated seek. On cache hit,
  the decoder still resumes with target-discard enabled to the end of the cached
  segment, so cached PCM does not get followed by duplicate pre-resume decoder
  output.
- **Seek hit accuracy**: PCM cache segment timestamps now advance from
  `targetPositionMs` by the number of decoded bytes already written, instead of
  stamping every segment with the seek target. This should make raw-stream cache
  hits closer to normal seek behavior.
- **Validation**:
  - Build: `./scripts/build-app.ps1 -BuildDir build-mm -Configuration Debug` -
    PASS.
  - Harness self-test:
    `./scripts/test-harness-reports.ps1 -BuildDir build-mm -SelfTest` - PASS.
  - Minimal playback regression:
    `./scripts/run-playback-regression.ps1 -BuildDir build-mm -Configuration Debug -CaseFilter wav-play-stop -NoCleanup` -
    PASS, report
    `E:\AI\OpenCode\AudioPlayer\build-mm\cache\logs\playback-regression-20260606-010236-855-bf29d898.json`.
  - MLP seek gate:
    `./scripts/run-playback-regression.ps1 -BuildDir build-mm -Configuration Debug -CaseFilter mlp-seek -NoCleanup` -
    PASS, seek resume latency improved from 3180 ms on the old sidecar to
    176 ms on the tight-cluster sidecar. Report
    `E:\AI\OpenCode\AudioPlayer\build-mm\cache\logs\playback-regression-20260606-010822-704-4443cfb0.json`.
  - MLP seek gate, sidecar reuse:
    `./scripts/run-playback-regression.ps1 -BuildDir build-mm -Configuration Debug -CaseFilter mlp-seek -NoCleanup` -
    PASS with `preparePlaybackSource reuse-sidecar`, seek resume latency
    209 ms. Report
    `E:\AI\OpenCode\AudioPlayer\build-mm\cache\logs\playback-regression-20260606-011700-507-a6333073.json`.
  - Ordinary WAV double-seek:
    `./scripts/run-playback-regression.ps1 -BuildDir build-mm -Configuration Debug -CaseFilter wav-double-seek -NoCleanup` -
    PASS, seek resume latency 142 ms. Report
    `E:\AI\OpenCode\AudioPlayer\build-mm\cache\logs\playback-regression-20260606-010853-957-6cdcc561.json`.
  - WASAPI exclusive MLP seek:
    `./scripts/run-playback-smoke.ps1 -Source "media\POWDER SNOW Live V9.8.6.mlp" -BuildDir build-mm -Configuration Debug -QuitAfterMs 10000 -SeekAfterMs 4000 -SeekToMs 30000 -RequirePlaying -RequireSeekCompletion -ExpectedSeekTargetMs 30000 -RejectPlaybackErrors -ExclusiveMode` -
    PASS, seek resume latency 401 ms, loaded backend `WASAPI exclusive`,
    submitted/render mirror clean. Report
    `E:\AI\OpenCode\AudioPlayer\build-mm\cache\logs\player-smoke-20260606-011106-451-25b90a71.report.json`.
  - WASAPI exclusive ordinary WAV seek:
    `./scripts/run-playback-smoke.ps1 -Source "build-mm\fixtures\smoke.wav" -BuildDir build-mm -Configuration Debug -QuitAfterMs 10000 -SeekAfterMs 4000 -SeekToMs 7000 -RequirePlaying -RequireSeekCompletion -ExpectedSeekTargetMs 7000 -RejectPlaybackErrors -ExclusiveMode` -
    PASS, seek resume latency 375 ms, loaded backend `WASAPI exclusive`,
    submitted/render mirror clean. Report
    `E:\AI\OpenCode\AudioPlayer\build-mm\cache\logs\player-smoke-20260606-011200-614-bd52d1e0.report.json`.
  - Far MLP seek, WASAPI shared:
    `./scripts/run-playback-smoke.ps1 -Source "media\POWDER SNOW Live V9.8.6.mlp" -BuildDir build-mm -Configuration Debug -QuitAfterMs 10000 -SeekAfterMs 4000 -SeekToMs 400000 -RequirePlaying -RequireSeekCompletion -ExpectedSeekTargetMs 400000 -RejectPlaybackErrors` -
    PASS, seek resume latency 210 ms, no buffer underrun/backpressure. Report
    `E:\AI\OpenCode\AudioPlayer\build-mm\cache\logs\player-smoke-20260606-011919-102-2119be72.report.json`.
  - Near-end MLP seek, WASAPI shared:
    `./scripts/run-playback-smoke.ps1 -Source "media\POWDER SNOW Live V9.8.6.mlp" -BuildDir build-mm -Configuration Debug -QuitAfterMs 12000 -SeekAfterMs 4000 -SeekToMs 445000 -RequirePlaying -RequireSeekCompletion -ExpectedSeekTargetMs 445000 -RejectPlaybackErrors` -
    PASS, seek resume latency 238 ms, no buffer underrun/backpressure. Report
    `E:\AI\OpenCode\AudioPlayer\build-mm\cache\logs\player-smoke-20260606-011943-531-0976c8e2.report.json`.
  - Near-end MLP seek, WASAPI exclusive:
    `./scripts/run-playback-smoke.ps1 -Source "media\POWDER SNOW Live V9.8.6.mlp" -BuildDir build-mm -Configuration Debug -ExclusiveMode -QuitAfterMs 5300 -SeekAfterMs 4000 -SeekToMs 445000 -RequirePlaying -RequireSeekCompletion -RequireSeekCompletionCount 1 -ExpectedSeekTargetMs 445000 -RejectPlaybackErrors` -
    PASS, seek resume latency 438 ms, loaded backend `WASAPI exclusive`,
    submitted/render mirror clean, no buffer underrun/backpressure. Report
    `E:\AI\OpenCode\AudioPlayer\build-mm\cache\logs\player-smoke-20260606-012114-423-9a1ed83a.report.json`.
  - Fresh/default raw Dolby PCM cache:
    `./scripts/run-playback-smoke.ps1 -Source "media\POWDER SNOW Live V9.8.6.mlp" -BuildDir build-mm -Configuration Debug -QuitAfterMs 9000 -SeekAfterMs 4000 -SeekToMs 30000 -RequirePlaying -RequireSeekCompletion -ExpectedSeekTargetMs 30000 -RejectPlaybackErrors -NoCleanup` -
    PASS after the sidecar-aware eligibility fix, with
    `pcmseekcache storageMode=memory capacityMiB=64` and seek resume latency
    183 ms. Report
    `E:\AI\OpenCode\AudioPlayer\build-mm\cache\logs\player-smoke-20260606-012544-216-1ed94603.report.json`.
  - Repeated MLP seek cache-hit quality, WASAPI shared:
    `./scripts/run-playback-smoke.ps1 -Source "media\POWDER SNOW Live V9.8.6.mlp" -BuildDir build-mm -Configuration Debug -QuitAfterMs 11000 -SeekSequence "4000:30000,7000:30000" -RequirePlaying -RequireSeekCompletion -RequireSeekCompletionCount 2 -RejectPlaybackErrors -NoCleanup` -
    PASS; after segment coalescing the second seek hit `cacheDurationMs=1000`
    and `cacheBytes=384000`, with seek resume latency 144 ms. Report
    `E:\AI\OpenCode\AudioPlayer\build-mm\cache\logs\player-smoke-20260606-013342-929-08de50e7.report.json`.
  - Repeated MLP seek cache-hit quality, WASAPI exclusive:
    `./scripts/run-playback-smoke.ps1 -Source "media\POWDER SNOW Live V9.8.6.mlp" -BuildDir build-mm -Configuration Debug -ExclusiveMode -QuitAfterMs 11000 -SeekSequence "4000:30000,7000:30000" -RequirePlaying -RequireSeekCompletion -RequireSeekCompletionCount 2 -RejectPlaybackErrors -NoCleanup` -
    PASS; the second seek hit `cacheDurationMs=1000` and
    `cacheBytes=384000`, with seek resume latency 407 ms. Report
    `E:\AI\OpenCode\AudioPlayer\build-mm\cache\logs\player-smoke-20260606-013510-782-fcb5d730.report.json`.
  - Mixed backward/forward MLP seek sequence, WASAPI shared:
    `./scripts/run-playback-smoke.ps1 -Source "media\POWDER SNOW Live V9.8.6.mlp" -BuildDir build-mm -Configuration Debug -QuitAfterMs 13000 -SeekSequence "4000:30000,7000:29000,10000:60000" -RequirePlaying -RequireSeekCompletion -RequireSeekCompletionCount 3 -RejectPlaybackErrors -NoCleanup` -
    PASS, latest seek resume latency 234 ms, no buffer underrun/backpressure.
    Report
    `E:\AI\OpenCode\AudioPlayer\build-mm\cache\logs\player-smoke-20260606-013342-949-1b5bdefe.report.json`.
  - Mixed backward/forward MLP seek sequence, WASAPI exclusive:
    `./scripts/run-playback-smoke.ps1 -Source "media\POWDER SNOW Live V9.8.6.mlp" -BuildDir build-mm -Configuration Debug -ExclusiveMode -QuitAfterMs 13000 -SeekSequence "4000:30000,7000:29000,10000:60000" -RequirePlaying -RequireSeekCompletion -RequireSeekCompletionCount 3 -RejectPlaybackErrors -NoCleanup` -
    PASS, latest seek resume latency 481 ms, no buffer underrun/backpressure.
    Report
    `E:\AI\OpenCode\AudioPlayer\build-mm\cache\logs\player-smoke-20260606-013406-437-64ac979a.report.json`.
  - Ordinary long M4A far seek baseline:
    shared 300s seek was INCONCLUSIVE because the detector flagged a compressed
    music seek-boundary candidate, but seek resume latency was 174 ms with no
    underrun/backpressure. Report
    `E:\AI\OpenCode\AudioPlayer\build-mm\cache\logs\player-smoke-20260606-011943-564-54beedbc.report.json`.
    Exclusive 300s seek had the same evidence limit, latency 386 ms, no
    underrun/backpressure. Report
    `E:\AI\OpenCode\AudioPlayer\build-mm\cache\logs\player-smoke-20260606-012133-698-e570e89e.report.json`.
- **Evidence limits**: The local MLP seek run passed at the
  scripted-playback/submitted-PCM layer and is now close to ordinary WAV seek
  latency in both shared regression and WASAPI exclusive smoke runs. Far and
  near-end raw MLP seek also remained in the hundreds-of-milliseconds range,
  comparable to the ordinary long M4A baseline on this workstation. The
  fresh/default raw-Dolby 64 MiB PCM seek-cache path is now validated for the
  sidecar playback path, and repeated seeks produce 1-second cache hits instead
  of zero-duration fragments. A user listening check reported seek-rebuild
  playback as still not fully stable, so cache-hit handoff now keeps decoder
  target-discard enabled through the cached segment end before appending decoder
  output. Endpoint acoustic output remains unproven in this pass. An attempted
  `sine-seek-resume-repeat` baseline failed because the harness observed zero
  `seekCompleted` events; it was not used as parity evidence.

- **Bug under investigation**: WASAPI exclusive playback of long MLP/TrueHD
  multi-channel content could stall around the point where PCM seek cache memory
  reached the previous 256 MiB default. Reported downstream symptom was
  `wasapiError hr=0xffffffff88890016 mappedError=4`.
- **Root-cause scope**: `LibavSeekDecoderWorker::appendPcm()` writes PCM seek
  cache segments synchronously on the decoder thread. The in-memory
  `PcmSeekCache` prune path moved large cached PCM ranges when the memory cap
  was reached, which can starve the playback buffer.
- **Fix**: PCM seek cache is now disabled by default for new/default settings
  (`maxPcmCacheMiB=0`). If a user explicitly enables the memory cache, writes
  stop once the memory cap is reached; existing cached seek hits are retained,
  but the decoder thread no longer prunes or compacts the in-memory cache in the
  write path.
- **Validation**:
  - Build: `./scripts/build-app.ps1 -BuildDir build-mm -Configuration Debug` -
    PASS.
  - Harness self-test:
    `./scripts/test-harness-reports.ps1 -BuildDir build-mm -SelfTest` - PASS.
  - Minimal playback regression:
    `./scripts/run-playback-regression.ps1 -BuildDir build-mm -Configuration Debug -CaseFilter wav-play-stop -NoCleanup` -
    PASS, report
    `E:\AI\OpenCode\AudioPlayer\build-mm\cache\logs\playback-regression-20260606-005016-628-052a6537.json`.
- **Evidence limits**: This removes the identified synchronous large-memory
  operation and confirms a basic WASAPI shared playback path still passes. It
  does not prove endpoint audio is pop/click-free, and no local long MLP
  exclusive-mode run was performed in this pass.

## Status refresh: 2026-05-19

- This tracker is split out from the WASAPI anomaly file so playback-cache and
  source-preparation work has its own evidence trail.
- No active playback-cache regression is consolidated in this file yet.
- The relevant implementation areas are `src/core/playbacksourceservice*`,
  `src/ui/mainwindow.*` cache settings UI, and cache-related script path
  handling under `scripts/`.
- Cache observations should not be treated as playback backend faults unless
  logs show the cache/source-preparation layer handing bad state to a backend.

## Current focus

- Track sidecar remux reuse/build failures, cache pruning, cache settings UI,
  diagnostic WAV retention, loopback cache retention, and path override issues.
- For local media or generated artifacts, keep large samples and generated cache
  outputs out of git unless explicitly requested.
- When a playback symptom involves cached sidecars, first record whether the run
  reused an existing sidecar or built a new one.

## Current priority order

1. Keep cache/source-preparation issues separate from WASAPI and ASIO backend
   diagnosis.
2. Record exact source path, cache root, sidecar path, command, log/report paths,
   and whether the cache file existed before playback.
3. Prefer script or UI evidence before changing source-preparation behavior.

## Current validation baseline

- Use `scripts/build-app.ps1` for C++ changes.
- Use smoke runs with explicit source paths when validating source-preparation
  behavior.
- Inspect logs for `preparePlaybackSource`, cache reuse/build, pruning, and
  source-switch evidence before assigning blame to a backend.

## Current acceptance bar

- Cache issues identify whether the fault is UI settings, path resolution,
  sidecar generation, sidecar reuse, pruning, or generated-artifact retention.
- Fixes do not change playback backend behavior unless the evidence requires it.

## Dated notes

- 2026-05-19: Created this tracker. No active playback-cache issue is
  consolidated here yet.
