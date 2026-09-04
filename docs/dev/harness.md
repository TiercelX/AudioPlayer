# Harness and automation

Use checked-in PowerShell scripts under `scripts/` before ad-hoc build,
smoke-test, log inspection, or playback automation commands.

## Script roles

- `scripts/bootstrap-dev-env.ps1`: local development environment preflight and
  orchestration entry point. It checks Git, CMake, Qt, MSVC, MSYS2, MSYS2
  `make`/`nasm`/`pkgconf`, host FFmpeg, and the self-built audio-core
  `ffmpeg.exe`/`ffprobe.exe`/libav file set. It can then call the existing
  FFmpeg runtime build, app build, fixture generation, smoke, and regression
  scripts. It is a coordinator only; keep build and report behavior owned by the
  underlying scripts.
- `scripts/build-app.ps1`: configure/build the Qt app.
  It resolves the Qt MSVC prefix from explicit `-QtPrefix`, then
  `CMAKE_PREFIX_PATH`, then `CMakePresets.json`, then common local Qt install
  roots. The script passes the resolved prefix to CMake as
  `CMAKE_PREFIX_PATH`, so clean shell builds do not depend on a global Qt
  environment variable.
  It requires a complete self-built `runtime-with-ffprobe-msvc` audio-core root
  under the build tree, or an explicit `-FfmpegAudioCoreRoot` pointing at another
  complete self-built root. The required file set is bundled
  `ffmpeg.exe`/`ffprobe.exe` plus the libav headers and `.lib` or `lib*.a`
  libraries used by the in-process decoder. The script no longer accepts
  `-DeployFfmpegExecutable` or `-DeployFfprobeExecutable` as a fallback to
  arbitrary external tools. At runtime, explicit `disabled`/`none` tool
  environment values still disable that tool; otherwise app-local bundled tools
  are preferred before environment path overrides and `PATH`.
- `scripts/build-app-msvc.cmd`: Windows-native wrapper for agents or shells that
  have unreliable PowerShell/PTY behavior. It loads `VsDevCmd.bat`, prepends
  common Qt CMake/Ninja tool paths, runs `scripts/build-app.ps1`, and writes a
  copy of the complete output under `build-claude-logs/` before returning the
  underlying build exit code. From PowerShell, call it directly as
  `.\scripts\build-app-msvc.cmd -BuildDir <dir> -Configuration Debug`; from a
  cmd-compatible launcher, use `cmd.exe /d /s /c ""scripts\build-app-msvc.cmd"
  -BuildDir <dir> -Configuration Debug"`. Use this wrapper from Claude Code or
  Git Bash instead of trying to export MSVC `INCLUDE`/`LIB` paths manually.
- `scripts/ensure-playback-fixtures.ps1`: generate lightweight repeatable media
  fixtures under the build tree. This script needs a full fixture-generation
  FFmpeg build with encoders/muxers beyond the slim playback runtime, so it may
  use `AUDIOPLAYER_FFMPEG_PATH` or `PATH`; playback/build/smoke still prefer the
  self-built audio-core runtime.
- `scripts/run-playback-smoke.ps1`: single-case runner. It starts the app,
  passes CLI automation options, captures logs, reads the JSON report, and
  enforces assertions. It also writes a script-owned harness report. After a
  build, it launches the latest `playable/<Configuration>` bundle recorded by
  `LATEST.txt`, falling back to the direct build output only when no playable
  bundle is available. If the selected executable directory lacks Qt runtime
  DLLs, or the deployed bundle lacks bundled `ffmpeg.exe` / `ffprobe.exe`, the
  script fails before launch with a deployment-focused error. Use
  `-AsioOutputIndex <n>` to route a smoke run through the app CLI's
  `--asio-output-index <n>` path; the harness fails the run if ASIO selection
  is requested but the app does not confirm the ASIO backend before loading.
  ASIO smoke reports also include `backendEvidence` fields for selection,
  configure-output, active sample rate, preferred buffer size, prepare-output,
  active audio state, first buffer switch, submitted-output evidence status, and
  endpoint-output evidence status.
- `scripts/harness-common.ps1`: shared helpers for smoke harness process
  cleanup, app argument escaping, media duration probing, log key/value parsing,
  result normalization, and harness report writing.
- `scripts/run-loopback-manual-smoke.ps1`: sidecar endpoint-output runner. It
  starts `WasapiLoopbackCapture`, then runs the normal smoke script so a manual
  observation window has a synchronized WASAPI loopback WAV and JSON report.
  If the loopback capture client is invalidated by an endpoint/spatial-audio
  change, the capture tool finalizes the current segment, reopens the current
  default render endpoint, and continues writing additional segment WAV and
  segment JSON files until the requested capture window ends. The wrapper also
  writes a `*.summary.json` evidence aggregation report that links the smoke
  harness report, app report, loopback reports/WAVs, and manual observation.
- `scripts/run-playback-regression.ps1`: case matrix over smoke scenarios. It
  aggregates per-case harness reports into one regression report.
- `scripts/analyze-audio-artifacts.ps1`: log-driven artifact attribution. Use
  `-ReportFile <path>` when a machine-readable JSON summary is needed for
  DeepSeek, MiMo, or handoff review.
- `scripts/collect-playback-evidence.ps1`: gather the latest or specified
  smoke/log/report artifacts into one evidence directory, run the analyzer
  unless `-NoAnalyzer` is supplied, and write a `manifest.json` for handoff.
- `scripts/summarize-loopback-alignment.ps1`: align app JSONL diagnostic events
  with WASAPI loopback capture segments and report which critical events were
  covered, outside capture, or in capture gaps/interrupted segments.
- `scripts/test-harness-reports.ps1`: validate smoke harness and regression
  aggregate JSON reports against the script-owned report contract.
- `scripts/run-tests.ps1`: build and run unit tests via ctest. Handles MSVC
  environment, Qt prefix resolution, and Qt DLL PATH setup. Supports
  `-NoBuild` to skip building, `-Verbose` for detailed output,
  `-Configuration` for build type selection, and `-ReportFile` for custom JSON
  report output path. Default report: `build-mm/test-report.json`.
- `scripts/validate-all.ps1`: unified validation entry point. Runs unit tests,
  report schema self-test, and optionally smoke tests in sequence. Writes an
  aggregate `validation-report.json` to the build directory. Supports
  `-SkipUnitTests`, `-SkipSmoke`, `-SkipSchemaCheck`, `-NoBuild`, `-Verbose`.
  Use this as the single command to verify project health before committing.
- `scripts/show-latest-log.ps1` and `scripts/show-latest-anomalies.ps1`: quick
  inspection helpers.
- `scripts/find-test-artifacts.ps1`: search active logs/loopback files and the
  archive manifest by run token, file name, or path fragment when revisiting
  evidence from an older change.

## FFmpeg audio-core source management

- The self-built FFmpeg audio-core source lives in `build-mm/ffmpeg-src`.
- Before building, always check https://ffmpeg.org/releases/ for a newer
  version. Do NOT assume the existing source is up-to-date.
- Current source version: **8.1.1** (updated 2026-05-29).
- For tool calls requiring full FFmpeg features (e.g. `ffmpeg -f lavfi` for
  test fixture generation), use the winget-installed `Gyan.FFmpeg` or another
  full build. The self-built slim runtime does not include lavfi and other
  non-playback components.
- After upgrading source, run
  `scripts/build-ffmpeg-audio-core.ps1 -Profile runtime-with-ffprobe -Toolchain msvc -RunBuild`
  to rebuild the slim runtime.
- The audio-core build stamp includes the source `RELEASE` value and
  `configure` SHA-256. A matching-stamp skip prints `runtimeVersion`; verify
  that line after source upgrades so a stale installed runtime is not reused.

## Smoke result contract

- Smoke tests must report exactly one top-level result: `PASS`, `FAIL`, or
  `INCONCLUSIVE`.
- Do not use `WARN` as a top-level smoke result. Put warning context in separate
  fields such as `warnings`, `systemInvalidationDuringSwitch`, or
  `verificationLayer`.
- `PASS` means the scripted behavior completed and required assertions passed
  within the report's current evidence and verification layer.
- `FAIL` means the app crashed/hung, required events were missing, playback
  errors occurred, stale session/buffer access was detected, or captured evidence
  contradicted the assertion.
- `INCONCLUSIVE` means the run completed but the available evidence layer cannot
  prove the requested property.
- Clean internal levels, internal PCM, or submitted WASAPI PCM are not enough to
  pass actual endpoint pop/click verification without manual listening or WASAPI
  loopback capture.
- Artifact-monitor and render-mirror clean results only cover submitted PCM. They
  cannot prove that Windows Sonic, Dolby Atmos, endpoint APO processing, driver
  processing, or the physical endpoint output stayed pop/click free.

## Report contract

Each smoke run should produce:

- text log path;
- JSONL diagnostic log path;
- app JSON report path;
- script harness JSON report path;
- top-level `result`;
- original app report result when present;
- evidence layer / verification layer;
- explicit endpoint-output verification status, such as
  `endpointOutputVerified=false` when no manual listening or loopback capture was
  used;
- for smoke harness `schemaVersion=2`, `backendEvidence`, including backend
  start, submitted-output, endpoint-output, and backend-specific evidence
  limits;
- explicit booleans for requested actions and observed completions;
- when `-RequireStopFadeOut` is used, submitted-PCM stop-fade completion,
  submitted frame count, peak, and final submitted sample;
- failure, inconclusive, or warning reasons when applicable.

`scripts/run-playback-smoke.ps1` owns the top-level harness result. It should
preserve app report fields, but normalize app `WARN` to harness
`INCONCLUSIVE` and keep warning context in `warnings`.

`scripts/run-playback-regression.ps1` should run all selected cases when
possible, record each case's report paths, and write an aggregate report with
case counts. For gate cases, the aggregate result is `FAIL` if any case fails,
`INCONCLUSIVE` if no selected gate case ran or any gate case is inconclusive,
and `PASS` only when all executed gate cases pass. Evidence-only cases are
reported separately and do not determine the aggregate gate result. Each
aggregate case entry should include metadata fields for triage and delegation:
`category`, `caseRole`, `evidenceLayer`, `requiresLocalMedia`,
`usesGeneratedFixture`, and `expectedInconclusiveReason`. The aggregate should
also include category and role summaries so DeepSeek, MiMo, or human reviewers
can quickly separate gate failures from evidence-only or local-media findings.

Prefer adding structured JSONL diagnostic events and script-side report parsing
over adding more fragile text-only assertions.

For ASIO selection/start runs, `backendEvidence.scope` should distinguish
selection/start evidence from submitted-output or endpoint-output evidence. A
successful ASIO selection/start run can verify driver configure/prepare/Active
state/first-buffer-switch while still reporting `submittedOutputVerified=false`,
`endpointOutputVerified=false`, and `result=INCONCLUSIVE` when no ASIO-specific
submitted-output capture or endpoint-output evidence exists.

Use `scripts/test-harness-reports.ps1` after changing report-writing scripts.
With no explicit `-Path`, it validates the latest smoke harness report. Use
`-LatestRegression`, or pass report paths directly, when validating regression
aggregate output:

```powershell
scripts/test-harness-reports.ps1 -LatestSmoke
scripts/test-harness-reports.ps1 -Path build-mm/cache/logs/playback-regression-example.json
```

Analyzer JSON reports and evidence bundles are handoff aids only. They summarize
existing local evidence and do not upgrade the evidence layer; endpoint-output
claims still require manual listening notes or WASAPI loopback evidence with the
same limitations described below.

## ChatGPT-only use

- ChatGPT/GitHub may inspect committed harness code, explain expected behavior,
  and review pasted logs or reports.
- Harness pass/fail claims must come from local `scripts/run-playback-smoke.ps1`
  or `scripts/run-playback-regression.ps1` output, or from user-provided report
  artifacts.
- Without local report/log evidence, conclusions should be labeled as a plan,
  hypothesis, or review finding, not `PASS`.

## Automation options

Playback automation should stay scriptable and report-oriented. Prefer CLI
options for source selection, output-device listing, initial system or ASIO
output selection, scheduled output switching, repeated switching, forced output
refresh, source switching, explicit log path, and explicit report path.

For expensive review or triage work, prefer producing a compact evidence bundle
first:

```powershell
scripts/collect-playback-evidence.ps1 -BuildDir build-mm
```

Use `-LogPath`, `-HarnessReportFile`, `-AppReportFile`, `-RegressionReportFile`,
or `-LoopbackReportFile` to pin a specific run. Use `-IncludeLoopbackWavs` only
when raw endpoint capture audio is needed; the default keeps bundles smaller and
copies loopback JSON metadata without WAV payloads. The bundle is intended for
DeepSeek log triage, Claude Code handoff, or human review, not as a new source
of validation.

When a bundle includes both JSONL diagnostics and a loopback aggregate report,
`scripts/collect-playback-evidence.ps1` also writes
`loopback-alignment.report.json`. This report is a timing index only: it can show
whether critical app events landed inside a loopback segment, in a capture gap,
before/after capture, or inside an interrupted segment. It still keeps
`endpointOutputVerified=false` and cannot prove pop-free endpoint output.

## Development environment bootstrap

Use `scripts/bootstrap-dev-env.ps1 -CheckOnly` after cloning or moving the repo
to verify local prerequisites without changing build outputs. The check reports
the resolved Qt prefix and expected audio-core runtime paths so missing local
state is visible before a long build starts.

Common flows:

```powershell
scripts/bootstrap-dev-env.ps1 -CheckOnly

scripts/bootstrap-dev-env.ps1 -BuildFfmpeg -BuildApp -Configuration Release

scripts/bootstrap-dev-env.ps1 `
  -BuildFfmpeg `
  -BuildApp `
  -EnsureFixtures `
  -RunTests `
  -RunSmoke `
  -BuildDir build-mm `
  -Configuration Release
```

Pass `-QtPrefix <path>` when Qt is installed outside the preset or common
locations. Pass `-VcVars64Path` or `-Msys2ShellPath` when Visual Studio or MSYS2
uses a nonstandard path. The bootstrap script does not install third-party tools
by default; it reports the missing prerequisite and lets the caller install or
point to the correct local copy.

## Delegation targets

Good DeepSeek MCP tasks:

- summarize `analyzer.report.json`, `manifest.json`, and selected log excerpts;
- draft a harness or report-contract plan;
- review a focused script diff;
- triage long smoke, regression, or loopback report output.

Good Claude Code tasks:

- implement script-only harness improvements on a Claude Code main-model branch
  such as `MiMo-*`, `DeepSeek-*`, or `Claude-*`;
- run local smoke/regression commands and report exact outputs;
- add report-schema or failure-path tests;
- prepare a branch with committed, validated harness work.

Codex/local remains responsible for final repository judgment when it is the
active agent. DeepSeek MCP output is advisory. Claude Code output can claim local
validation only when Claude Code actually ran the local command or inspected the
local artifact.

## Path environment overrides

- `AUDIOPLAYER_BUILD_DIR`: default build tree for scripts when `-BuildDir` is
  omitted. CLI `-BuildDir` still wins.
- `AUDIOPLAYER_CACHE_DIR`: default cache root for scripts and app runtime when
  no explicit script path is supplied. Scripts put logs under `logs` and
  loopback captures under `loopback` inside this root.
- `AUDIOPLAYER_LOG_DIR`: default script/app log directory for generated
  `player-*.log` files. `AUDIOPLAYER_LOG_FILE` still wins when a single app log
  file is required.
- `AUDIOPLAYER_VCVARS64_PATH` and `AUDIOPLAYER_MSYS2_SHELL_PATH`: default
  local tool paths used by `scripts/build-ffmpeg-audio-core.ps1 -RunBuild`
  when the matching CLI parameters are omitted.
- `AUDIOPLAYER_FFMPEG_SOURCE_DIR` and
  `AUDIOPLAYER_FFMPEG_AUDIO_CORE_PREFIX`: default source and install
  directories for `scripts/build-ffmpeg-audio-core.ps1 -RunBuild`.

For seek-resume testing, `scripts/run-playback-smoke.ps1` supports
`-SeekPauseResume` to route scripted seeks through the same pause -> seek ->
play path used by progress-slider release. Use `-SeekSequence` with comma
separated `afterMs:targetMs` entries for repeated seek-resume runs in one app
session, and pair it with `-RequireSeekCompletionCount` plus
`-RequireSeekResumeProfileCount` when asserting the profile path.

When using `-RequireFinished`, the smoke script reads the source duration with
`ffprobe` before launching the app. It automatically extends the effective quit
timeout to at least `duration + -RequireFinishedBufferMs` (default 5000 ms). If
duration cannot be determined from `ffprobe`, the script fails before launch
instead of running a guaranteed-failing finished assertion.

For seek-resume artifact checks, synthetic silence and sine fixtures are hard
assertions: any submitted-PCM artifact on those sources is a regression unless a
newer diagnostic explains otherwise. The required ALAC seek-resume gate uses a
generated tone fixture, `sine-1khz-minus18db-48k-stereo-alac.m4a`, rather than a
song-derived sample. Real song/music samples such as `real-alac-sample.m4a` are
local compatibility/manual evidence only, not stable required gates or hard
artifact oracles. Whole-segment transient counting on music can flag normal
musical attacks, so judge future ALAC seek bugs by manual audibility plus
immediate seek-resume boundary-window evidence, not by `artifactDetected` alone.

Use `scripts/run-playback-regression.ps1 -IncludeLocalMediaEvidence` when
collecting local compatibility evidence such as `real-alac-sample.m4a`.
Evidence-only cases are recorded in the aggregate report but do not determine
the top-level regression result.

Render-mirror raw/json artifacts are written per playback session because a
seek, source switch, active-switch rebuild, or recovery creates a fresh WASAPI
session/buffer generation. Keep the per-session files when investigating
submitted PCM boundaries; use the script retention controls (`-KeepRuns`,
`-NoCleanup`) to manage the active log/cache directories. By default, retention
archives older smoke/regression/loopback artifacts under
`cache/test-artifact-archive/<yyyyMM>/<run>/` and appends lookup metadata to
`cache/test-artifact-archive/manifest.jsonl` instead of deleting old runs.

## Manual loopback smoke guidance

- Use `scripts/run-loopback-manual-smoke.ps1 -Source <path>` for the media
  source. The current wrapper parameter is `-Source`, not `-SourcePath`.
- `-ManualEndpointResult` is optional. Omit it when the manual result is
  unclear, when the run is evidence-gathering only, or when the observation is
  better described in `-ManualObservationNote`.
- Include the switch direction in `-ManualObservationNote`, such as
  `Off to Windows Sonic` or `Windows Sonic to Off`.
- Include the Windows endpoint/system volume and any relevant device/app volume
  in `-ManualObservationNote`. Volume is a reproducibility factor; do not treat
  it as root cause unless later evidence proves that.
- Label the manual observation in `-ManualObservationNote` as one or more of:
  `pre-playback residue`, `switch pop`, `jitter/warble/shaking`, or
  `no audible issue`.
- Use silence or tone fixtures for first-pass manual checks. Start with
  `silence-48k-stereo.wav`, then try `sine-1khz-minus18db-48k-stereo.wav` or
  `sine-1khz-minus12db-48k-stereo.wav` when `sine-1khz-minus30db-48k-stereo.wav`
  is too quiet at the selected endpoint volume. Do not use pink noise as the
  first manual check because it can mask transient clicks.
- Loopback WAV/report output is sidecar evidence only. It is synchronized with
  the smoke run, but it is not merged into the app JSON report contract.
- The wrapper-level `*.summary.json` is an evidence aggregation layer only. It
  keeps `endpointOutputVerified=false` and `interpretation.canClaimNoPop=false`;
  manual `NoPop` input remains `manualObservation.result`, and zero loopback
  transient candidates are not promoted to endpoint verification.
- The loopback sidecar reports resumed near-silence dropout candidates in
  addition to transient candidates. It also reports a low-level-tone
  `tailFadeCandidateObserved` field for trailing fade-to-silence triage. This is
  candidate evidence only: shared endpoint capture can contain audio from
  other applications, and a clean candidate count is not acoustic proof.
- If spatial switching invalidates the loopback capture client, preserve the
  interrupted multi-segment sidecar report. A zero loopback transient count is
  `INCONCLUSIVE` when the capture was interrupted at the critical switch window.

## Media and artifacts

- Keep large local media samples out of the main git history unless the user
  explicitly asks for a different asset strategy.
- Prefer generated or checked-in lightweight fixtures for repeatable tests.
- Treat generated silence/sine fixtures as hard pass/fail sources for
  seek-resume submitted-PCM assertions. Treat real ALAC/music samples as
  optional local compatibility/listening evidence where full-window short-burst,
  crackle, or transient counts can be content-shaped false positives.
- For endpoint loopback pop/click checks, prefer generated silence or tone
  fixtures such as `silence-48k-stereo.wav`,
  `sine-1khz-minus30db-48k-stereo.wav`,
  `sine-1khz-minus18db-48k-stereo.wav`, and
  `sine-1khz-minus12db-48k-stereo.wav`. Pink noise is useful for subjective
  continuity checks but can mask light transient spikes in first-pass loopback
  detection, so do not use it as the first manual check.
- Keep build outputs, reports, raw captures, and large local media in ignored
  build/cache/dist/media locations unless a task explicitly changes that policy.
- Keep loopback WAV/report sidecars under the build cache by default. They are
  endpoint-layer evidence and should not be merged into the app report unless a
  later task explicitly changes the report contract.
- Multi-segment loopback runs write one aggregate `loopback-*.report.json` plus
  per-segment `loopback-*-segmentN.wav` / `loopback-*-segmentN.report.json`
  sidecars. Aggregate detector silence is `INCONCLUSIVE`, not proof that the
  endpoint path was pop/click free.
