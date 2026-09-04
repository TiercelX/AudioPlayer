# Diagnostics

Use this page when adding or interpreting playback logs, smoke reports, or
pop/click evidence.

## Evidence layers

- Treat decoder-side PCM, internal audio levels, submitted backend PCM, and
  actual output-device audio as different evidence layers.
- `audioLevelsChanged`, internal PCM monitors, and submitted PCM mirrors only
  prove what the player produced before or submitted to the OS/audio backend.
- `AudioArtifactMonitor` and the WASAPI render mirror are submitted-PCM-layer
  checks. They can show whether PCM passed to `IAudioRenderClient` is clean, but
  they do not observe Windows Sonic, Dolby Atmos, endpoint APO, driver, or
  speaker/headphone output after the WASAPI shared engine.
- Internal PCM checks do not prove actual speaker/headphone output is pop-free.
- Manual listening or output loopback capture is required when the claim is about
  actual speaker/headphone output.
- WASAPI loopback capture is endpoint-layer evidence, but first-pass spike
  and resumed near-silence dropout detection are not complete pop/click
  classifiers. Treat their candidates as strong leads, and treat clean results
  as limited evidence rather than proof of perfect output continuity.
- Low-level-tone loopback runs can report a `tailFadeCandidateObserved` field
  when recent audible packets descend into trailing silence. This supports
  stop-fade triage, but shared endpoint capture can contain unrelated
  application audio and the field is not acoustic proof.
- Pink noise can hide light pop/click transients from simple peak/delta
  detectors. Use silence or a low-level tone fixture when the goal is to make
  endpoint-layer transient spikes easier to see in loopback capture.
- For seek-resume submitted-PCM checks, keep the immediate boundary window
  separate from full-segment music content. Synthetic silence/sine fixtures are
  hard assertions. Real compressed music samples can contain normal short
  bursts, crackle-like textures, or large musical transients later in the
  segment; classify those as content-transient leads unless they land in the
  seek-resume boundary window or have stale PCM, hard discontinuity, invalid
  sample/timestamp, impossible jump, or render-mirror mismatch evidence.

## Evidence sources

- GitHub-only evidence means committed source and documentation.
- Local evidence means build output, generated text logs, JSONL diagnostics,
  smoke reports, raw captures, or local untracked files from this workspace.
- Endpoint evidence means manual listening or loopback capture of actual
  speaker/headphone output.
- Do not turn source-code reasoning into a runtime or endpoint-output claim.

## Pop/click claims

- Do not continue guessing fixes for audible pops/clicks without improving
  observability first.
- Do not claim an audible pop/click is fixed unless the relevant automated
  diagnostic report passes, the limitation of the test layer is stated, and
  manual listening or output loopback confirms the actual output path when
  applicable.
- If clean playback is observed only below the endpoint-output layer, report
  pop/click verification as `INCONCLUSIVE`.

## Logging policy

- Preserve per-launch log rotation: one log file per app launch, timestamp plus
  PID in the file name. Keep the newest logs in the active log directory and
  archive older logs under `logs/archive/<yyyyMM>/` instead of deleting them.
- `AUDIOPLAYER_LOG_FILE` forces a single app log file. Without it,
  `AUDIOPLAYER_LOG_DIR` overrides the generated log directory, and
  `AUDIOPLAYER_CACHE_DIR` overrides the cache root whose `logs` child is used.
- Keep structured playback logs with category and thread ID fields unless the
  task explicitly changes logging requirements.
- Prefer structured JSONL diagnostic events in addition to human-readable logs
  when adding or changing diagnostics.
- Routine app launches keep high-frequency JSONL burst diagnostics disabled by
  default. Set `AUDIOPLAYER_HIGH_VOLUME_JSONL_DIAGNOSTICS=1` when investigating
  decoder burst cadence or internal PCM discontinuity details that would
  otherwise dominate the JSONL log.
- For WASAPI playback, ffmpeg decode is paced in real time by default to keep
  Qt stdout buffering from running far ahead of the PCM ring during Debug
  playback and seek tests. `SeekResume` keeps `-re` but allows a bounded
  `-readrate_initial_burst 1.500` after the target is fixed, so ALAC seek
  release can prime the startup buffer without switching to unpaced or faster
  steady-state decode. Set `AUDIOPLAYER_FFMPEG_REALTIME_DECODE=0` only when
  explicitly investigating decoder throughput or backpressure behavior.
- Validated packaged codecs on the WASAPI backend can bypass the ffmpeg CLI and
  use the in-process libav decoder worker (`decoderMode=libav-inprocess`). The
  current default libav route covers AAC, ALAC, FLAC, MP3, and PCM WAV variants.
  Although the self-built audio-core runtime also contains Dolby decoders such
  as AC3, E-AC-3, and TrueHD, those formats stay on the ffmpeg CLI route until
  their libav worker boundary behavior is validated cleanly. Set
  `AUDIOPLAYER_WASAPI_LIBAV_DECODER=0` to force the older ffmpeg CLI path; the
  legacy `AUDIOPLAYER_WASAPI_LIBAV_SEEK_RESUME=0` switch is still honored as a
  libav opt-out. This is a decoder/feed-path change, not endpoint-output proof.
- WASAPI render-mirror captures default to the startup window used by the
  checked-in diagnostics. Set `AUDIOPLAYER_WASAPI_RENDER_MIRROR_WINDOW_MS` to a
  bounded value such as `6000` when investigating audible artifacts that occur a
  few seconds after playback starts. This expands the submitted-PCM evidence
  window only; it still does not prove endpoint-output behavior without
  loopback/manual evidence.

## Output-switch diagnostics

Output-switch diagnostics should record:

- switch requested/start/done;
- source session/generation id;
- playback position;
- previous and target output device id/name;
- output format;
- sink/backend state transitions;
- sink/backend errors;
- buffer underrun/starvation events when available;
- whether the event occurred during active output switching or recovery.

When changing switch or recovery semantics, keep scripts and logs able to
distinguish active switching, recovery, hot-reconfigure hits, and conservative
rebuild fallback.

## Report fields

Smoke/app reports should keep these classes distinguishable when the evidence is
available:

- `activeOutputSwitchDetected` / `activeOutputSwitchStartedCount` /
  `activeOutputSwitchCompletedCount`;
- `sameOutputInvalidationDetected` / `sameOutputInvalidationCount`, plus
  absorbed invalidation event counts when present;
- `wasapiErrorRecoveryDetected` / `wasapiErrorRecoveryScheduledCount` /
  `wasapiErrorRecoveryStartCount`;
- `staleBufferReuseDetected`, plus stale session-write and buffer-read counts;
- `bufferUnderrunDetected` / `bufferUnderrunCount`;
- `submittedPcmDiscontinuityDetected`, submitted PCM metric-block counts, and
  max submitted PCM peak/jump values;
- `artifactMonitorCandidateCount` separately from soft
  `activeSwitchBoundaryPopCandidate` telemetry when available;
- `actualEndpointOutputVerification` and `endpointOutputVerified`.
- Stop-fade fields when available, including `stopFadeOutCompleted`,
  `stopFadeOutSubmittedFrames`, `stopFadeOutMaxSubmittedPeak`, and
  `stopFadeOutLastSubmittedSample`;
- Seek-resume artifact classification fields when available, including
  `artifactWindow`, `artifactClassification`,
  `firstArtifactOffsetMsAfterResume`, `seekResumeBoundaryWindowMs`, and
  `compressedContentSample`;
- Seek-resume first submitted block fields when available, including
  `firstSubmittedBlockPeak`, `firstSubmittedBlockStartSample`,
  `firstSubmittedBlockEndSample`, `firstSubmittedBlockFadeApplied`,
  `firstSubmittedBlockMinGain`, `firstSubmittedBlockMaxGain`,
  `first50msSubmittedPcmJumpAfterSeek`, and
  `renderMirrorFirst50msAfterSeekArtifactDetected`.

These report fields are diagnostic labels for the current evidence layer. They
do not prove actual endpoint output is pop-free without manual listening or
loopback capture.
