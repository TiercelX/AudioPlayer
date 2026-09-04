# Harness and report status

This file tracks smoke/regression/loopback harness reports, evidence bundles,
report schema, result contracts, and automation reliability. Keep durable
workflow rules in `AGENTS.md` and `docs/dev/*.md`.

## Status refresh: 2026-09-01 (FFmpeg 9.0.1 audio-core build and validation)

- The self-built audio-core target is now pinned to FFmpeg `9.0.1` in both
  Windows and Linux build scripts. The Windows flow clones tag `n9.0.1`,
  rejects an existing source tree with another `RELEASE`, and `build-app.ps1`
  plus CMake reject bundled tools that do not report FFmpeg 9.0.1.
- The distribution profile remains slim: file/pipe I/O, the app's audio
  demuxers and decoders, PCM encoders/muxers, `aformat`/`aresample`/
  `channelmap`/`pan`, and the required parsers only. No video, network,
  avdevice, shared-library, or full upstream feature set was added.
- Shared runtime lookup now checks app-local tools, then an explicit file or
  directory from `AUDIOPLAYER_FFMPEG_PATH` / `AUDIOPLAYER_FFPROBE_PATH`, then
  the matching global PATH tool. Global tools are fallback-only and are never
  copied into the package.
- Validation: the pinned runtime was built with
  `scripts\build-ffmpeg-audio-core.ps1 -Profile runtime-with-ffprobe
  -Toolchain msvc -SourceDir build-mm\ffmpeg-src-9.0.1 -PrefixDir
  build-mm\ffmpeg-audio-core\runtime-with-ffprobe-msvc -RunBuild -Jobs 8`.
  Both bundled tools report `n9.0.1`. The Debug app package build passed at
  `build-mm\playable\Debug\20260901-204252\AudioPlayer.exe`.
- Unit validation is `PASS`: `scripts\run-tests.ps1 -BuildDir build-mm
  -Configuration Debug -NoBuild -Verbose -ReportFile
  build-mm\test-report-ffmpeg-901.json`; all 10 suites and both CTest entries
  passed. The already-installed global FFmpeg was not used to satisfy the
  bundled-runtime gate.
- Current recommendation: no additional FFmpeg modules are justified for the
  guaranteed playback profile. The next useful build addition is a separate
  fixture/dev profile for formats such as Opus/Vorbis/APE/WavPack, with
  regression coverage; do not enlarge the distribution profile for that.

## Durable guidance

- Bug/status tracking index: `docs/bug/README.md`
- Workflow and change scope: `docs/dev/agent-workflow.md`
- Harness and smoke-test policy: `docs/dev/harness.md`
- Diagnostics and evidence layers: `docs/dev/diagnostics.md`
- Handoff prompt templates: `docs/dev/handoff-templates.md`
- Git and release workflow: `docs/dev/release-workflow.md`

## Status refresh: 2026-07-17 (codex-0630 Release test package)

- Built commit `a123666` (`Add WASAPI spatial static bed path`) as a Windows
  x64 Release bundle with
  `scripts\build-app-msvc.cmd -BuildDir build-package-0630 -Configuration Release -FfmpegAudioCoreRoot build-mm\ffmpeg-audio-core\runtime-with-ffprobe-msvc`.
  The build passed and deployed
  `build-package-0630\playable\Release\20260717-151427\AudioPlayer.exe` with
  the required Qt runtime, platform plugin, bundled `ffmpeg.exe`, and bundled
  `ffprobe.exe`.
- Release unit tests passed all 10 suites with
  `scripts\run-tests.ps1 -BuildDir build-package-0630 -Configuration Release -NoBuild -ReportFile build-package-0630\test-report-package.json`.
- Deployed-bundle WASAPI shared smoke passed with
  `scripts\run-playback-smoke.ps1 -Source build-mm\fixtures\silence-48k-stereo.wav -BuildDir build-package-0630 -Configuration Release -QuitAfterMs 5000 -RequirePlaying -RejectPlaybackErrors`.
  Harness report:
  `build-package-0630\cache\logs\package-smoke.harness.json`; app report:
  `build-package-0630\cache\logs\package-smoke.app.json`.
- Packaged local test artifact:
  `dist\AudioPlayer-codex-0630-a123666-win64-Release-20260717.zip`, SHA-256
  `298792ADCE37E20299F9928790ADED728B3F57369B1B0BA6C86FDE5FA8ABB185`.
  The ZIP and build/report directories are ignored local artifacts and are not
  tracked in Git.
- Evidence limit: build, unit tests, deployment completeness, application
  startup, source load, WASAPI shared playback state, and clean process exit
  are `PASS`. Actual endpoint output and pop/click behavior remain
  `INCONCLUSIVE` because this run used silence without manual listening or
  WASAPI loopback capture.

## Status refresh: 2026-06-30 (new Windows setup and fixture FFmpeg path)

- New-machine setup on `codex-0630` built the self-built FFmpeg audio-core
  runtime from official FFmpeg tag `n8.1.2` under
  `build-mm\ffmpeg-audio-core\runtime-with-ffprobe-msvc`.
- Release app build passed and deployed
  `build-mm\playable\Release\20260630-020909\AudioPlayer.exe` with bundled
  self-built `ffmpeg.exe` / `ffprobe.exe`; the bundled tool reports
  `ffmpeg version n8.1.2`.
- Release unit tests passed with all 10 suites:
  `scripts\run-tests.ps1 -BuildDir build-mm -Configuration Release -QtPrefix
  C:\Qt\6.11.1\msvc2022_64 -NoBuild -Verbose`; report:
  `build-mm\test-report.json`.
- Installed Gyan FFmpeg 8.1.2 full build only as the host fixture-generation
  tool and set `AUDIOPLAYER_FFMPEG_PATH` to its `ffmpeg.exe`. This full build
  is not accepted as a deploy-time replacement by `scripts\build-app.ps1`.
- Bootstrap preflight now resolves host FFmpeg through `AUDIOPLAYER_FFMPEG_PATH`
  before falling back to `PATH`, matching `scripts\ensure-playback-fixtures.ps1`.
  The app build and deploy path still require the complete self-built
  `runtime-with-ffprobe-msvc` audio-core root.

## Status refresh: 2026-06-02 (merged codex stop-fade and validation harness)

- **Branch merge**: Merged `codex-0601-wasapi-bitdepth` into `opencode-0528`.
  The merge includes stop-fade and validation harness improvements from
  codex commit `5b28560`.
- **Changes merged**:
  - Smoke option `-RequireStopFadeOut` for completed PCM stop fade validation
  - Loopback dropout detection improvements
  - Cold-start monitor false positive fix
  - Output-refresh gate false positive fix
  - Stale FFmpeg audio-core cache reuse fix
- **Validation**: Merge completed; harness self-test should be run to verify.
- **Status update**: This merge brings the opencode branch current with
  codex's harness and validation improvements.
- **Branch**: `opencode-0528` (current working branch).

## Status refresh: 2026-06-01 (stop-fade and loopback dropout coverage)

- Added smoke option `-RequireStopFadeOut`. It requires a completed
  sample-level PCM stop fade with zero final gain and records submitted fade
  frames, max submitted peak, and last submitted sample. Default regression
  `play-stop` cases that stop active playback now enable this assertion; the
  pause-without-resume stop case intentionally does not.
- Fixed `scripts\run-loopback-manual-smoke.ps1` named argument forwarding.
  Array splatting previously passed `-RequirePlaying` positionally as
  `SeekAfterMs`; wrapper pass-through now parses named switches and values.
- The WASAPI loopback sidecar now fails on resumed near-silence dropout
  candidates, while ignoring leading silence and reporting trailing silence
  separately. It also reports low-level-tone `tailFadeCandidateObserved` as a
  triage field, not endpoint acoustic proof.
- Fixed a cold-start monitor false positive exposed by the full regression:
  generated `smoke.m4a` has a normal AAC priming overshoot near `0.1633`, but
  the artifact monitor treated every `firstDataBlockAfterConfigure` as a
  sensitive switch/recovery context. Plain `NormalStart` now uses normal burst
  thresholds; switch, recovery, device-rebuild, and unlabelled ASIO first-block
  paths remain sensitive.
- Fixed an output-refresh gate false positive exposed by a second full
  regression: `activeSwitchBoundaryPopCandidate` can vary with the synthetic
  sine phase while a protected bridge fade is submitted. The candidate remains
  logged and counted for triage, but no longer fails a synthetic-fixture gate
  by itself. Render-mirror artifacts, artifact-monitor candidates, stale data,
  and backend failures still fail the gate.
- Fixed stale FFmpeg audio-core cache reuse: build stamps now include source
  `RELEASE` and `configure` SHA-256, and stamp hits print `runtimeVersion`.
  The isolated `8.1.1` runtime skip check printed
  `runtimeVersion:ffmpeg version 8.1.1`.
- Validation PASS: `scripts\test-harness-reports.ps1 -SelfTest`.
- Validation PASS: default regression after the monitor fixes executed 14 gate
  cases successfully and skipped 7 local-media cases whose `media\*.eb3/.mlp`
  inputs were absent:
  `build-codex-validation-ffmpeg811\cache\logs\playback-regression-20260601-122121-835-a842237c.json`.
- Shared tone loopback smoke app assertions PASS with wrapper result
  `INCONCLUSIVE`, as intended for candidate-only endpoint evidence:
  `build-codex-validation-ffmpeg811\cache\logs\player-loopback-smoke-20260601-122535-335-de3275c0.summary.json`.
  The sidecar reported zero transient candidates, zero dropout candidates,
  `tailFadeCandidateObserved=true`, and 460 ms trailing silence. Alignment
  summary:
  `build-codex-validation-ffmpeg811\cache\loopback\loopback-20260601-122535-335-de3275c0.alignment.report.json`.

## Status refresh: 2026-05-28

- Codex follow-up: `scripts\run-playback-smoke.ps1` was reduced to 254 lines by
  moving harness report construction into `Write-SmokeHarnessReport` and console
  diagnostic-output printing into `Write-SmokeDiagnosticOutput` in
  `scripts\playback-smoke-assertions.ps1`. Public smoke CLI options and harness
  report fields are intended to remain unchanged.
- Codex added `docs\dev\opencode-structure-prework.md` as the pre-roadmap
  handoff for opencode/MiMo structure-split work, and linked it from
  `docs\dev\opencode-workflow.md` and
  `docs\dev\opencode-four-day-roadmap.md`.
- Codex validation: `git diff --check -- scripts/run-playback-smoke.ps1
  scripts/playback-smoke-assertions.ps1 docs/dev/opencode-structure-prework.md
  docs/dev/opencode-workflow.md docs/dev/opencode-four-day-roadmap.md` passed.
- Codex validation: `scripts\test-harness-reports.ps1 -SelfTest` passed:
  `selfTest:PASS`.
- Codex validation: default `scripts\test-harness-reports.ps1 -LatestSmoke`
  was not usable because `D:\AI\Codex\AudioPlayer\build-mm\cache\logs` had no
  harness report; rerun with `-BuildDir build-structure-automation-codex`
  passed against
  `D:\AI\Codex\AudioPlayer\build-structure-automation-codex\cache\logs\player-smoke-20260528-065922-715-57f478d6.harness.json`.
- Codex validation: WASAPI shared smoke passed with
  `scripts\run-playback-smoke.ps1 -BuildDir build-structure-automation-codex
  -Configuration Debug -Source
  D:\AI\Codex\AudioPlayer\build-codex-asio-format-fallback\fixtures\sine-1khz-352800-stereo-s24.wav
  -QuitAfterMs 3000 -RequirePlaying -RejectPlaybackErrors`. Harness report:
  `D:\AI\Codex\AudioPlayer\build-structure-automation-codex\cache\logs\player-smoke-20260528-072208-644-8b56922c.harness.json`;
  `harnessResult:PASS`, `assertions:ok`.
- Codex validation: `scripts\test-harness-reports.ps1 -LatestSmoke -BuildDir
  build-structure-automation-codex` passed:
  `harness:PASS:...8b56922c.harness.json`. Endpoint-output verification
  remains `INCONCLUSIVE` (scripted playback/submitted-PCM evidence only).
- Codex adopted Claude Code branch `claude-0523` into `codex-0416` by
  fast-forward after reviewing the `Claude-0528` structure split commits. The
  adopted scope is limited to the smoke runner/assertion split and CLI
  automation option extraction.
- Codex validation: `scripts\test-harness-reports.ps1 -SelfTest` passed after
  adoption.
- Codex validation: Debug build passed with
  `scripts\build-app-msvc.cmd -BuildDir build-structure-automation-codex
  -Configuration Debug -FfmpegAudioCoreRoot
  D:\AI\Codex\AudioPlayer-codex-0523-asio\build-mm\ffmpeg-audio-core\runtime-with-ffprobe-msvc`.
  Build log:
  `D:\AI\Codex\AudioPlayer\build-claude-logs\build-app-msvc-74b74c46e771439588c6f44cca171aed.log`.
  Playable app:
  `D:\AI\Codex\AudioPlayer\build-structure-automation-codex\playable\Debug\20260528-065911\AudioPlayer.exe`.
- Codex validation: WASAPI shared smoke passed with
  `scripts\run-playback-smoke.ps1 -BuildDir build-structure-automation-codex
  -Configuration Debug -Source
  D:\AI\Codex\AudioPlayer\build-codex-asio-format-fallback\fixtures\sine-1khz-352800-stereo-s24.wav
  -QuitAfterMs 3000 -RequirePlaying -RejectPlaybackErrors`. Harness report:
  `D:\AI\Codex\AudioPlayer\build-structure-automation-codex\cache\logs\player-smoke-20260528-065922-715-57f478d6.harness.json`;
  `harnessResult:PASS`, `assertions:ok`.
- Codex validation: `scripts\test-harness-reports.ps1 -LatestSmoke -BuildDir
  build-structure-automation-codex` passed:
  `harness:PASS:...57f478d6.harness.json`. Endpoint-output verification
  remains `INCONCLUSIVE` (scripted playback/submitted-PCM evidence only).
- Smoke runner script split (worktree Claude-0528 validation): assertion and
  runner helpers extracted into `scripts\playback-smoke-assertions.ps1` and
  `scripts\playback-smoke-runner.ps1`; `scripts\run-playback-smoke.ps1` reduced
  to ~491 lines of orchestration. Public CLI options and harness report schema
  unchanged.
- Validation: `scripts\test-harness-reports.ps1 -SelfTest` passed (worktree
  Claude-0528, `build-structure-lowrisk`).
- Validation: Debug build passed with
  `scripts\build-app-msvc.cmd -BuildDir build-structure-lowrisk -Configuration
  Debug -FfmpegAudioCoreRoot
  D:\AI\Codex\AudioPlayer-codex-0523-asio\build-mm\ffmpeg-audio-core\runtime-with-ffprobe-msvc`.
  Build log:
  `D:\AI\Claude Code\AudioPlayer\.claude\worktrees\Claude-0528\build-claude-logs\build-app-msvc-4010a874627d43c4b95fac956e5499df.log`.
  Playable app:
  `build-structure-lowrisk\playable\Debug\20260528-052626\AudioPlayer.exe`.
- Validation: WASAPI shared smoke passed with
  `scripts\run-playback-smoke.ps1 -BuildDir build-structure-lowrisk
  -Configuration Debug -Source
  D:\AI\Codex\AudioPlayer\build-codex-asio-format-fallback\fixtures\sine-1khz-352800-stereo-s24.wav
  -QuitAfterMs 3000 -RequirePlaying -RejectPlaybackErrors`. Harness report:
  `D:\AI\Claude Code\AudioPlayer\.claude\worktrees\Claude-0528\build-structure-lowrisk\cache\logs\player-smoke-20260528-052646-865-ae302124.harness.json`;
  `harnessResult:PASS`, `assertions:ok`.
- Validation: `scripts\test-harness-reports.ps1 -LatestSmoke -BuildDir
  build-structure-lowrisk` passed:
  `harness:PASS:...ae302124.harness.json`. Endpoint-output verification remains
  `INCONCLUSIVE` (scripted playback/submitted-PCM evidence only).
- Smoke runner script split: `scripts\run-playback-smoke.ps1` further reduced
  by extracting assertion/result-normalization logic into
  `scripts\playback-smoke-assertions.ps1` (`Invoke-SmokeAssertions`) and
  path/parameter-validation/process-launch logic into
  `scripts\playback-smoke-runner.ps1` (`Invoke-SmokeRunner`,
  `Write-EarlyFailureReport`). The main script now contains only top-level
  parameters, module dot-sourcing, orchestration calls, observedActions
  construction, harness report writing, diagnostic output, and cleanup. Public
  CLI options and harness report schema are unchanged.
- Validation: `scripts\test-harness-reports.ps1 -SelfTest` passed.
- Validation: WASAPI shared smoke passed using the existing
  `build-mimo-asio` Debug playable bundle:
  `scripts\run-playback-smoke.ps1 -BuildDir
  D:\AI\Claude Code\AudioPlayer\build-mimo-asio -Configuration Debug -Source
  D:\AI\Codex\AudioPlayer\build-codex-asio-format-fallback\fixtures\sine-1khz-352800-stereo-s24.wav
  -QuitAfterMs 3000 -RequirePlaying -RejectPlaybackErrors`. Harness report:
  `D:\AI\Claude Code\AudioPlayer\build-mimo-asio\cache\logs\player-smoke-20260528-050343-552-0f5a82e0.harness.json`;
  `harnessResult:PASS`, `assertions:ok`.
- Validation: `scripts\test-harness-reports.ps1 -LatestSmoke -BuildDir
  D:\AI\Claude Code\AudioPlayer\build-mimo-asio` passed:
  `harness:PASS:...0f5a82e0.harness.json`. Endpoint-output verification remains
  `INCONCLUSIVE` (scripted playback/submitted-PCM evidence only).
- CLI automation options split (Task 2, worktree Claude-0528, commit 294f5cb):
  `src\ui\main.cpp` reduced to 33 lines of startup orchestration; CLI option
  parsing and scheduled automation setup moved to `src\ui\automationoptions.h`
  and `src\ui\automationoptions.cpp`; module added to `CMakeLists.txt`.
- Validation: Debug build passed with
  `scripts\build-app-msvc.cmd -BuildDir build-structure-automation -Configuration
  Debug -FfmpegAudioCoreRoot
  D:\AI\Codex\AudioPlayer-codex-0523-asio\build-mm\ffmpeg-audio-core\runtime-with-ffprobe-msvc`.
  Build log:
  `D:\AI\Claude Code\AudioPlayer\.claude\worktrees\Claude-0528\build-claude-logs\build-app-msvc-801914c2a9be48a794337d8f57f9e7c6.log`.
  Playable app:
  `build-structure-automation\playable\Debug\20260528-053525\AudioPlayer.exe`.
- Validation: WASAPI shared smoke passed with
  `scripts\run-playback-smoke.ps1 -BuildDir build-structure-automation
  -Configuration Debug -Source
  D:\AI\Codex\AudioPlayer\build-codex-asio-format-fallback\fixtures\sine-1khz-352800-stereo-s24.wav
  -QuitAfterMs 3000 -RequirePlaying -RejectPlaybackErrors`. Harness report:
  `D:\AI\Claude Code\AudioPlayer\.claude\worktrees\Claude-0528\build-structure-automation\cache\logs\player-smoke-20260528-053544-004-1cb56e8f.harness.json`;
  `harnessResult:PASS`, `assertions:ok`.
- Validation: `scripts\test-harness-reports.ps1 -LatestSmoke -BuildDir
  build-structure-automation` passed:
  `harness:PASS:...1cb56e8f.harness.json`. Endpoint-output verification remains
  `INCONCLUSIVE` (scripted playback/submitted-PCM evidence only).
- Structure split follow-up: CLI diagnostic report construction moved from
  `src\ui\main.cpp` into `src\ui\diagnosticreportbuilder.cpp`, and main-window
  UI implementation was split into adjacent responsibility files for output
  selection, automation, media, cache settings, and shared UI helpers. The
  public CLI options, report schema, backend behavior, and `MainWindow` public
  API are intended to remain unchanged.
- Validation: Debug build passed with
  `scripts\build-app-msvc.cmd -BuildDir build-structure-lowrisk -Configuration
  Debug -FfmpegAudioCoreRoot
  D:\AI\Codex\AudioPlayer-codex-0523-asio\build-mm\ffmpeg-audio-core\runtime-with-ffprobe-msvc`.
  Build log:
  `D:\AI\Codex\AudioPlayer\build-claude-logs\build-app-msvc-5cb1c89a90fb459581f5e7a3094bae8c.log`.
- Validation: WASAPI shared smoke passed with
  `scripts\run-playback-smoke.ps1 -BuildDir build-structure-lowrisk
  -Configuration Debug -Source
  D:\AI\Codex\AudioPlayer\build-codex-asio-format-fallback\fixtures\sine-1khz-352800-stereo-s24.wav
  -QuitAfterMs 3000 -RequirePlaying -RejectPlaybackErrors`. Harness report:
  `D:\AI\Codex\AudioPlayer\build-structure-lowrisk\cache\logs\player-smoke-20260528-030750-691-3f794abb.harness.json`;
  schema check passed with
  `scripts\test-harness-reports.ps1 -Path ...3f794abb.harness.json`. This is
  scripted playback/submitted-PCM evidence only; endpoint-output verification
  remains `INCONCLUSIVE`.
- Smoke harness structure follow-up: `scripts\run-playback-smoke.ps1` now
  dot-sources `scripts\playback-smoke-evidence.ps1` for post-run log parsing,
  backend evidence shaping, evidence-layer labels, and ASIO submitted-output
  evidence fields. The public smoke script parameters and harness report schema
  are intended to remain unchanged.
- Validation: `scripts\test-harness-reports.ps1 -SelfTest` passed after the
  helper extraction. A WASAPI shared smoke also passed using the existing Debug
  playable bundle:
  `scripts\run-playback-smoke.ps1 -BuildDir
  D:\AI\Codex\AudioPlayer\build-codex-asio-format-fallback -Configuration Debug
  -Source
  D:\AI\Codex\AudioPlayer\build-codex-asio-format-fallback\fixtures\sine-1khz-352800-stereo-s24.wav
  -QuitAfterMs 3000 -RequirePlaying -RejectPlaybackErrors`. Harness report:
  `D:\AI\Codex\AudioPlayer\build-codex-asio-format-fallback\cache\logs\player-smoke-20260528-023732-482-492d36bc.harness.json`;
  schema check passed with
  `scripts\test-harness-reports.ps1 -Path ...492d36bc.harness.json`. This is
  scripted playback/submitted-PCM evidence only; endpoint-output verification
  remains `INCONCLUSIVE`. The extracted helper was also exercised against the
  historical ASIO log/report pair
  `player-smoke-20260526-025251-700-94337e3c` and parsed
  `backend=Windows ASIO`, `startVerified=True`, and `submitted=True`; that
  check covers parser behavior only and is not a fresh ASIO playback validation.
- Build contract integration after the `codex-0416` / `codex/asio-format-fallback`
  branch comparison: kept the current self-built audio-core tool requirement for
  bundled `ffmpeg.exe`/`ffprobe.exe`, and split libav enforcement into
  `AUDIOPLAYER_REQUIRE_LIBAV_DECODER`. CMake now resolves libav libraries as
  either MSVC `.lib` files or `lib*.a` archives before enabling
  `AUDIOPLAYER_LIBAV_DECODER`.
- `scripts\build-app.ps1` now passes both
  `-DAUDIOPLAYER_REQUIRE_FFMPEG_AUDIO_CORE=ON` and
  `-DAUDIOPLAYER_REQUIRE_LIBAV_DECODER=ON`, so default app builds still fail
  fast when either bundled tools or in-process decoder files are missing.
- Routine high-volume JSONL events (`decoder_read_burst`,
  `libav_decoder_read_burst`, and `internal_pcm_glitch_monitor`) are disabled by
  default. Set `AUDIOPLAYER_HIGH_VOLUME_JSONL_DIAGNOSTICS=1` for investigations
  that need those per-burst details.
- Validation: Debug build passed with
  `cmd.exe /d /s /c ""scripts\build-app-msvc.cmd" -BuildDir build-codex-asio-format-fallback
  -Configuration Debug -FfmpegAudioCoreRoot
  D:\AI\Codex\AudioPlayer-codex-0523-asio\build-mm\ffmpeg-audio-core\runtime-with-ffprobe-msvc"`.
  Build log:
  `build-claude-logs\build-app-msvc-056e4eccee4a423ebc91df46f8807806.log`.
  Playable app:
  `build-codex-asio-format-fallback\playable\Debug\20260528-014030\AudioPlayer.exe`.
- Validation: WASAPI shared smoke passed with
  `scripts\run-playback-smoke.ps1 -Source build-codex-asio-format-fallback\fixtures\sine-1khz-352800-stereo-s24.wav
  -BuildDir build-codex-asio-format-fallback -Configuration Debug -QuitAfterMs
  3000 -RequirePlaying -RejectPlaybackErrors`. Harness report:
  `build-codex-asio-format-fallback\cache\logs\player-smoke-20260528-014046-634-2ada0e98.harness.json`;
  schema check passed with `scripts\test-harness-reports.ps1 -Path ...2ada0e98.harness.json`.
  The smoke JSONL contained no high-volume burst/glitch events with the default
  environment. Endpoint-output verification remains `INCONCLUSIVE`.

## Status refresh: 2026-05-26

- Build contract tightened: default Windows app builds now require a complete
  self-built `runtime-with-ffprobe-msvc` FFmpeg audio-core root containing
  `bin\ffmpeg.exe`, `bin\ffprobe.exe`, libav headers, and MSVC import libraries.
  `scripts\build-app.ps1` rejects `-DeployFfmpegExecutable` and
  `-DeployFfprobeExecutable` instead of allowing arbitrary full upstream tools
  into default playable bundles. Use `-FfmpegAudioCoreRoot` only to point at
  another complete self-built audio-core root.
- CMake now enforces the same Windows default with
  `AUDIOPLAYER_REQUIRE_FFMPEG_AUDIO_CORE=ON`: libav is no longer silently
  optional, and explicit deploy tool paths must resolve to
  `AUDIOPLAYER_FFMPEG_AUDIO_CORE_ROOT\bin\ffmpeg.exe` / `ffprobe.exe`.
- Deploy packaging keeps Qt Multimedia's FFmpeg runtime pruned even when the
  in-process libav decoder is enabled. Debug packages now remove
  `multimedia\ffmpegmediaplugind.dll` in addition to the release plugin name and
  the Qt `avcodec`/`avformat`/`avutil`/`swresample`/`swscale` DLLs.
- Validation: old external-tool build command rejected immediately:
  `scripts\build-app.ps1 -BuildDir build-codex-asio-format-fallback
  -Configuration Debug -DeployFfmpegExecutable D:\Tool\ffmpeg\bin\ffmpeg.exe
  -DeployFfprobeExecutable D:\Tool\ffmpeg\bin\ffprobe.exe`.
- Validation: MSVC Debug build passed with a complete self-built audio-core root
  from
  `D:\AI\Codex\AudioPlayer-codex-0523-asio\build-mm\ffmpeg-audio-core\runtime-with-ffprobe-msvc`.
  Build log:
  `build-claude-logs\build-app-msvc-7399746394354f81b5089f78cab2b95d.log`.
  Playable app:
  `build-codex-asio-format-fallback\playable\Debug\20260526-023830\AudioPlayer.exe`.
  Package inspection showed only bundled `ffmpeg.exe` and `ffprobe.exe` from
  that root, plus `windowsmediaplugind.dll`; Qt FFmpeg plugin/DLL payloads were
  absent after pruning.
- Validation: direct CMake configure under an MSVC environment rejected an
  external `D:\Tool\ffmpeg\bin\ffmpeg.exe` deploy path even when the audio-core
  root was otherwise complete.
- Validation: no-override WASAPI smoke passed using the app-local bundled tools:
  `scripts\run-playback-smoke.ps1 -Source
  build-codex-asio-format-fallback\fixtures\sine-1khz-352800-stereo-s24.wav
  -BuildDir build-codex-asio-format-fallback -Configuration Debug -QuitAfterMs
  3000 -RequirePlaying -RejectPlaybackErrors`. Harness report:
  `build-codex-asio-format-fallback\cache\logs\player-smoke-20260526-023930-426-915d7408.harness.json`;
  schema check passed with `scripts\test-harness-reports.ps1 -Path ...915d7408.harness.json`.

## Status refresh: 2026-05-25

- Added performance-oriented build configurations without changing the default
  Debug click path. `build-mimo-asio.cmd` now accepts an optional configuration
  argument, with helper click scripts `build-mimo-asio-relwithdebinfo.cmd` and
  `build-mimo-asio-release.cmd`. The click script rejects unsupported
  configuration names before invoking CMake. `scripts\build-app.ps1`,
  `scripts\bootstrap-dev-env.ps1`, and smoke/regression/loopback wrappers now
  accept `RelWithDebInfo`. `build-app.ps1` also passes
  `CMAKE_BUILD_TYPE=<Configuration>` so the NMake single-config generator
  actually builds the requested configuration.
- Added `AUDIOPLAYER_ENABLE_MSVC_RELEASE_LTCG=ON` for MSVC Release-like app
  builds and `AUDIOPLAYER_ENABLE_AVX2=OFF` as an explicit opt-in. Release and
  RelWithDebInfo app builds now receive `/GL` at compile time and
  `/LTCG /OPT:REF /OPT:ICF` at link time; AVX2 remains disabled by default for
  portable x64 bundles.
- Validation: the first `build-mimo-asio-relwithdebinfo.cmd` attempt exposed
  that NMake was still deploying Debug because `CMAKE_BUILD_TYPE` was unset.
  After the script fix, `cmd.exe /d /s /c "echo y|
  build-mimo-asio-relwithdebinfo.cmd"` passed and produced
  `build-mimo-asio\playable\RelWithDebInfo\20260525-031137\AudioPlayer.exe`.
  Log: `build-claude-logs\build-app-msvc-7ab917f2246644a8ba1056fa6a02f7d7.log`.
- Validation: `cmd.exe /d /s /c "echo y| build-mimo-asio-release.cmd"` passed
  and produced
  `build-mimo-asio\playable\Release\20260525-031253\AudioPlayer.exe`. Log:
  `build-claude-logs\build-app-msvc-3dd978347e3e42899446dc60caaf0018.log`.
  Generated build files confirm `/GL` in
  `build-mimo-asio\CMakeFiles\AudioPlayer.dir\flags.make` and
  `/LTCG /OPT:REF /OPT:ICF` in
  `build-mimo-asio\CMakeFiles\AudioPlayer.dir\build.make`; cache confirms
  `AUDIOPLAYER_ENABLE_AVX2:BOOL=OFF`.
- Validation: `cmd.exe /d /s /c "build-mimo-asio.cmd Nope"` rejected the
  unsupported configuration before any build work.
- Improved the `build-mimo-asio.cmd` double-click experience without changing
  the audio-core build preference. The click script now writes the FFmpeg
  source/runtime preparation stage to a separate GUID-named
  `build-claude-logs\build-ffmpeg-audio-core-*.log`, prints stage-oriented
  progress in the console, summarizes `buildSkipped:` / `builtPrefix:`, detects
  incomplete `build-mimo-asio\ffmpeg-src` checkouts before invoking configure,
  and prints the latest playable Debug bundle path on success.
- Validation: `cmd.exe /d /s /c "echo y| build-mimo-asio.cmd"` passed with the
  improved console flow. FFmpeg runtime prep skipped via matching profile stamp
  and wrote
  `build-claude-logs\build-ffmpeg-audio-core-a33713208b5044c1a7b9aac8045a568c.log`;
  app build wrote
  `build-claude-logs\build-app-msvc-fe7871ec461f409599095d6151743647.log` and
  printed playable bundle
  `D:/AI/Claude Code/AudioPlayer/build-mimo-asio/playable/Debug/20260525-030411`.
- Reproduced the root cause for the `build-mimo-asio.cmd` click-build failure
  reported from
  `build-claude-logs\build-app-msvc-b46493ab772f437c98d07f1bcf15b8c6.log`:
  `scripts\build-app.ps1` now strictly requires the self-built
  `ffmpeg.exe`/`ffprobe.exe` under the selected build tree, but the click script
  only invoked the app build. `build-mm` already had
  `ffmpeg-audio-core\runtime-with-ffprobe-msvc\bin\ffmpeg.exe` and
  `ffprobe.exe`; fresh `build-mimo-asio` did not.
- Corrected `build-mimo-asio.cmd` so the one-click flow follows the current
  MiMo handoff preference: use `build-mimo-asio\ffmpeg-src` and invoke
  `scripts\build-ffmpeg-audio-core.ps1 -Toolchain msvc -RunBuild` with
  `AUDIOPLAYER_BUILD_DIR=build-mimo-asio`. The script does not pass
  `-DisableX86Asm`, does not pass full-FFmpeg deploy overrides, and does not
  copy `build-mm` runtime binaries into the target build tree. If
  `build-mimo-asio\ffmpeg-src` is missing, the click script clones FFmpeg
  `n7.1` there before building.
- Validation: an earlier `cmd.exe /d /s /c "echo y| build-mimo-asio.cmd"` run
  passed after copying the `build-mm` runtime and wrote
  `build-claude-logs\build-app-msvc-66f8f86263364124b6acab2c187231de.log`.
  That run validated the app build path but not the corrected audio-core
  rebuild preference.
- Validation: `cmd.exe /d /s /c "echo y| build-mimo-asio.cmd"` then passed with
  the corrected audio-core path and wrote
  `build-claude-logs\build-app-msvc-8431904690f740fe9771a50508e6d28f.log`.
  The FFmpeg configure output used `install prefix
  /q/ffmpeg-audio-core/runtime-with-ffprobe-msvc`, reported `standalone assembly
  yes` and `x86 assembler nasm`, and built
  `build-mimo-asio\ffmpeg-audio-core\runtime-with-ffprobe-msvc`. The app build
  printed
  `exe:D:\AI\Claude Code\AudioPlayer\build-mimo-asio\playable\Debug\20260525-030035\AudioPlayer.exe`.
- Fixed two one-click runtime-prep script bugs found while validating the
  corrected path: `scripts\build-ffmpeg-audio-core.ps1` now falls back to a
  .NET SHA256 implementation if `Get-FileHash` cannot be resolved after loading
  the MSVC batch environment, and its SUBST path rewrite now compares MSYS-style
  `/d/...` paths so a workspace path containing `Claude Code` is rewritten to
  the temporary `/q/...` mount before FFmpeg configure runs.

## Status refresh: 2026-05-24

- Added root `build-mimo-asio.cmd` as a user-clickable build entry for the
  current 0523 ASIO Debug target. It calls
  `scripts\build-app-msvc.cmd -BuildDir build-mimo-asio -Configuration Debug`,
  reports success/failure, and pauses before closing so a double-clicked window
  remains readable.
- The click script deletes the intermediate `build-mimo-asio\AudioPlayer.exe`
  before invoking the normal build wrapper. This forces relink/post-build deploy
  on no-op incremental builds so each successful click creates a fresh
  `build-mimo-asio\playable\Debug\<timestamp>` directory for manual testing.
- Validation: `cmd /c "echo y| build-mimo-asio.cmd"` advanced `LATEST.txt` from
  `build-mimo-asio\playable\Debug\20260524-104756` to
  `build-mimo-asio\playable\Debug\20260524-115537` and wrote
  `build-claude-logs\build-app-msvc-211220aad47449a8aae0185ce044ea41.log`.
- Validation: `cmd /c "echo y| build-mimo-asio.cmd"` passed and wrote
  `build-claude-logs\build-app-msvc-b84f1d53a55c473ca00dd9dd5fe9297c.log`
  with `args=-BuildDir build-mimo-asio -Configuration Debug` and
  `exe:D:\AI\Codex\AudioPlayer-codex-0523-asio\build-mimo-asio\playable\Debug\20260524-104756\AudioPlayer.exe`.
- Updated `CLAUDE.md` and `docs/dev/claude-code-workflow.md` so Claude Code does
  not attempt Windows MSVC builds when its session is Bash-only. In that case it
  must report the build as unvalidated and defer to the user-click script or
  Codex/local validation instead of wrapping `cmd.exe` or reading old logs.
- Added an explicit current-branch build contract to `CLAUDE.md` and
  `docs/dev/claude-code-workflow.md`: Claude Code command-line validation in
  `codex-0523-asio` should use only
  `.\scripts\build-app-msvc.cmd -BuildDir build-mimo-asio -Configuration Debug`,
  must verify the fresh `buildLog:` path and matching `args=...`, and must not
  judge the current task from `build-mm`, `build-codex-asio*`, or older logs.
  This narrows Claude/build validation without changing Codex/local's ability to
  use other checked-in scripts for explicit tasks.
- Fixed VSCode CMake Tools configure after reload by pinning
  `.vscode/settings.json` `cmake.cmakePath` to the Visual Studio 2026 bundled
  CMake at
  `D:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`.
  Qt Tools CMake on this machine does not know the `Visual Studio 18 2026`
  generator used by `CMakePresets.json`.
- Validation: Qt Tools CMake reproduced the failure with `CMake Error: Could not
  create named generator Visual Studio 18 2026`; Visual Studio bundled CMake
  `4.2.3-msvc3` recognized the generator. `cmake --preset windows-msvc-x64`
  and `cmake --build --preset debug` passed with the VS bundled CMake, producing
  `build\playable\Debug\20260524-105718\AudioPlayer.exe`. `windeployqt`
  emitted the known `VCINSTALLDIR is not set` warning but deployment completed.
- Hardened `scripts/build-app-msvc.cmd` log creation after concurrent wrapper
  launches reused the same `%RANDOM%`-based log name and produced intermittent
  `The process cannot access the file because it is being used by another
  process.` messages. Log names now use a GUID when PowerShell can provide one,
  with a multi-random fallback.
- Updated Claude-facing command examples to prefer direct PowerShell invocation
  (`.\scripts\build-app-msvc.cmd ...`) and document the safer
  `cmd.exe /d /s /c ""scripts\build-app-msvc.cmd" ..."` form for
  cmd-compatible launchers.
- Validation: both `.\scripts\build-app-msvc.cmd -BuildDir build-mimo-asio
  -Configuration Debug` and `cmd.exe /d /s /c ""scripts\build-app-msvc.cmd"
  -BuildDir build-mimo-asio -Configuration Debug"` passed in
  `D:\AI\Codex\AudioPlayer-codex-0523-asio`, writing GUID-named logs
  `build-app-msvc-d36fd68adf394ab5b5fbb15da5ef2797.log` and
  `build-app-msvc-6bf67b38607840bbbad1abe6a11e7ffd.log`.
- Added `scripts/build-app-msvc.cmd` as a Windows-native wrapper for Claude Code
  or other shells with unreliable PowerShell/PTY behavior. The wrapper loads
  `VsDevCmd.bat`, prepends common Qt CMake/Ninja tool paths, calls
  `scripts/build-app.ps1`, writes the full output under `build-claude-logs/`,
  echoes the log path, and returns the build exit code.
- Updated `docs/dev/harness.md` and `docs/dev/claude-code-workflow.md` to route
  Claude Code Windows builds through `cmd /c scripts\build-app-msvc.cmd ...`
  instead of Git Bash `INCLUDE`/`LIB` export attempts.
- Updated `CLAUDE.md` with Claude Code operating rules: Chinese explanations by
  default, stop-and-report triggers for repeated or silent environment failures,
  blocked-report content requirements, and the MSVC wrapper as the preferred
  Windows build path.
- Validation: `cmd /c scripts\build-app-msvc.cmd -BuildDir build-mimo-asio
  -Configuration Debug` passed, loaded VS 2026
  `D:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat`,
  found Qt CMake at `D:\Qt\Tools\CMake_64\bin\cmake.exe`, found MSVC `nmake.exe`,
  and wrote
  `build-claude-logs\build-app-msvc-9037-11561.log`.
- Fixed `scripts/build-app.ps1 -BuildDir <absolute path>` path handling. The
  script now resolves the build directory once before deriving the self-built
  ffmpeg/ffprobe runtime paths, passing `-B` to CMake, building, and resolving
  the deployed executable. This avoids paths like
  `repo\D:\...\build-mimo-asio\ffmpeg-audio-core\...`.
- Validation: `cmd /c build-mimo-asio.cmd` passed in
  `D:\AI\Codex\AudioPlayer-codex-0523-asio` and produced playable Debug bundle
  `build-mimo-asio\playable\Debug\20260524-092342\AudioPlayer.exe`. The direct
  build command `cmd /c tmp_build.cmd` also passed. The pre-fix absolute
  `-BuildDir` run failed at the duplicated ffmpeg path, confirming the
  reproduced script bug; the post-fix absolute `-BuildDir
  D:\AI\Codex\AudioPlayer-codex-0523-asio\build-mimo-asio` run passed.
- The observed MSVC `type_traits` failure is an environment-shell issue, not an
  ASIO source compile failure: the same `build-mimo-asio` cache builds when
  launched through `VsDevCmd.bat`/`cmd`, and `cl.exe` resolves the MSVC and
  Windows SDK include directories.

## Status refresh: 2026-05-23

- Fixed three bugs in `scripts/bootstrap-dev-env.ps1`:
  1. `Resolve-Msys2ShellPath` and `Resolve-VcVars64Path`: PowerShell
     `Where-Object` filtering a single-element `@()` array downgrades the result
     to `String`, causing subsequent `+=` to concatenate strings instead of
     appending to an array. Wrapped with `[System.Collections.ArrayList]`.
  2. `Resolve-VcVars64Path`: hardcoded search paths only covered C:\Program
     Files. Added D:\Program Files variants for machines with VS on D: drive.
  3. `Resolve-VcVars64Path`: `vswhere -products *` returns empty for VS 2026
     (version 18). Added fallback chains: `-latest` without `-products`, and
     `-all`.
- `scripts/build-app.ps1`: ffmpeg deploy now throws when the self-built
  `ffmpeg.exe` is not found, matching the existing strict behavior for
  `ffprobe.exe`. This prevents silent fallback to system-installed ffmpeg on
  fresh deployments.
- Installed MSYS2 2026-03-22 at `D:\msys64` with `make`, `nasm`, `pkgconf`.
- Downloaded FFmpeg 8.1.1 source tarball to `build-mm/ffmpeg-src`.
- Validation: `scripts/bootstrap-dev-env.ps1 -CheckOnly -Msys2ShellPath
  D:\msys64\msys2_shell.cmd` with refreshed PATH passes all prerequisites
  except audio-core ffmpeg/ffprobe (not yet built).
- FFmpeg 8.1.1 audio-core (runtime-with-ffprobe) built successfully with MSVC
  via `scripts\build-ffmpeg-audio-core.ps1 -Profile runtime-with-ffprobe
  -Toolchain msvc -RunBuild -DisableX86Asm`. Output:
  `build-mm\ffmpeg-audio-core\runtime-with-ffprobe-msvc\bin\ffmpeg.exe` (2.2MB),
  `ffprobe.exe` (1.9MB). Bootstrap now shows all 11 checks PASS.
- Build required `subst P:` to work around spaces in project path (`Claude
  Code`); MSYS2 bash splits unquoted paths at spaces. The build script's
  `Resolve-VcVars64Path` and `Resolve-Msys2ShellPath` were also patched for
  VS 2026 (vswhere `-products *` fallback) and D: drive MSYS2 fallback.

## Status refresh: 2026-05-21

- Development environment setup now has a script-owned bootstrap entry:
  `scripts/bootstrap-dev-env.ps1`. It reports local prerequisite status for Git,
  CMake, Qt, MSVC, MSYS2, MSYS2 `make`/`nasm`/`pkgconf`, host FFmpeg, and the
  self-built audio-core runtime paths, then optionally calls the existing
  FFmpeg build, app build, fixture, smoke, and regression scripts.
- `scripts/build-app.ps1` now resolves Qt explicitly from `-QtPrefix`,
  `CMAKE_PREFIX_PATH`, `CMakePresets.json`, or common local Qt install roots and
  passes the resolved value to CMake as `CMAKE_PREFIX_PATH`. This keeps clean
  script builds from depending on a pre-set shell environment variable.
- Validation: `scripts/bootstrap-dev-env.ps1 -CheckOnly` passed and found Qt at
  `D:\Qt\6.11.0\msvc2022_64`, VS 18 Build Tools
  `vcvars64.bat`, MSYS2, host FFmpeg, and the self-built audio-core
  `ffmpeg.exe`/`ffprobe.exe`; `scripts\build-app.ps1 -BuildDir build-mm
  -Configuration Debug` passed and printed
  `qtPrefix:D:\Qt\6.11.0\msvc2022_64`; `scripts\bootstrap-dev-env.ps1
  -BuildApp -BuildDir build-mm -Configuration Debug` passed and produced a
  playable Debug bundle under `build-mm\playable\Debug\20260521-062801`.
  `windeployqt` still warns that `VCINSTALLDIR` is not set when the shell has
  not imported the Visual Studio environment; the deployment completed.

- Log/test-artifact retention now archives instead of deleting older evidence:
  app launch logs move to `logs/archive/<yyyyMM>/`, and script-owned
  smoke/regression/loopback artifacts move to
  `cache/test-artifact-archive/<yyyyMM>/<run>/` with a JSONL manifest for later
  lookup by run token, file name, or documented report/log path.
- `scripts/find-test-artifacts.ps1` searches active logs plus the archive
  manifest so old validation evidence can be reopened after active retention
  has moved it out of `cache/logs` / `cache/loopback`.
- Validation: `scripts/test-harness-reports.ps1 -SelfTest` passed; a scoped
  temporary-cache retention probe with 3 fake runs and `-KeepRuns 1` archived 2
  runs / 8 files with `removedRuns=0` and `removedFiles=0`;
  `scripts/find-test-artifacts.ps1` found the archived log by run token;
  `scripts/build-app.ps1 -BuildDir build-mm -Configuration Debug` passed; a
  temp-cache app launch probe archived one old app `.log` plus its `.jsonl`
  companion under `logs/archive/<yyyyMM>/`.

- Smoke harness reports now use `schemaVersion=2` and include a top-level
  `backendEvidence` object. ASIO runs record requested/confirmed ASIO selection,
  configured driver/rate/channel fields, active sample rate, preferred buffer
  size, prepare-output observation, active audio state observation, first buffer
  switch observation, and explicit submitted-output / endpoint-output
  verification booleans. The schema validator still accepts legacy v1 reports
  without `backendEvidence`.
- The ASIO report scope now separates selection/start/driver-callback evidence
  from output evidence: `backendEvidence.scope=asio-selection-start-and-driver-callback`
  can coexist with `submittedOutputVerified=false`,
  `endpointOutputVerified=false`, and `harnessResult=INCONCLUSIVE`.
- Validation: `scripts/test-harness-reports.ps1 -SelfTest` passed; ASIO smoke
  using `-ListOutputDevices -AsioOutputIndex 1 -RequirePlaying
  -RejectPlaybackErrors` wrote harness report
  `build-mm/cache/logs/player-smoke-20260521-053703-461-bca36cb6.harness.json`
  with `loadedBackend=Windows ASIO`, selected `VB-Matrix VASIO-128`,
  `asioBackendStartVerified=True`, `activeSampleRate=44100`,
  `preferredBufferSize=512`, and `harnessResult=INCONCLUSIVE`; non-ASIO smoke
  using `-RequirePlaying -RejectPlaybackErrors` passed with harness report
  `build-mm/cache/logs/player-smoke-20260521-053712-641-5ce7be0a.harness.json`;
  `scripts/test-harness-reports.ps1 -LatestSmoke -Path
  build-mm/cache/logs/player-smoke-20260521-053703-461-bca36cb6.harness.json`
  passed for both reports.

- `scripts/run-playback-smoke.ps1` now exposes `-AsioOutputIndex <n>` and
  includes ASIO selection fields in smoke harness reports.
- The harness keeps the existing top-level result contract: `PASS`, `FAIL`, or
  `INCONCLUSIVE`. The new ASIO selection/start smoke can still be
  `INCONCLUSIVE` when endpoint output and ASIO-specific submitted-output
  evidence are not verified.
- Validation: `scripts/test-harness-reports.ps1 -SelfTest` passed; non-ASIO
  smoke using `-ListOutputDevices`, `-RequirePlaying`, and
  `-RejectPlaybackErrors` passed with harness report
  `build-mm/cache/logs/player-smoke-20260521-051622-326-f6554bda.harness.json`;
  ASIO smoke with `-AsioOutputIndex 1 -RequirePlaying -RejectPlaybackErrors`
  wrote harness report
  `build-mm/cache/logs/player-smoke-20260521-051722-206-48e1000e.harness.json`
  with `loadedBackend=Windows ASIO`, selected `VB-Matrix VASIO-128`, and
  `harnessResult=INCONCLUSIVE`; `scripts/test-harness-reports.ps1 -LatestSmoke`
  passed for that latest ASIO report.

## Status refresh: 2026-05-20

- `scripts/run-playback-smoke.ps1` now dot-sources shared helper functions from
  `scripts/harness-common.ps1`. The extraction covers process cleanup, app
  argument escaping, media duration probing, log key/value parsing, smoke result
  normalization, and harness report writing.
- The smoke runner remains the owner of single-case harness reports and the
  `PASS` / `FAIL` / `INCONCLUSIVE` top-level result contract.
- Validation: `scripts/test-harness-reports.ps1 -SelfTest` passed;
  the smoke command `scripts/run-playback-smoke.ps1` on
  `build-mm/fixtures/smoke.wav` with
  `-QuitAfterMs 3000 -RequirePlaying -RejectPlaybackErrors` passed with harness
  report
  `build-mm/cache/logs/player-smoke-20260520-031528-087-6daeb491.harness.json`;
  `scripts/test-harness-reports.ps1 -LatestSmoke` passed for that report.

## Status refresh: 2026-05-19

- This tracker is split out from the WASAPI anomaly file so harness/report work
  can evolve without making the WASAPI file a general project changelog.
- The current top-level result contract is `PASS`, `FAIL`, or `INCONCLUSIVE`.
  `WARN` must not be used as a top-level harness or regression result.
- App `WARN` is warning context; script-owned harness reports normalize it to
  `INCONCLUSIVE`.
- Submitted PCM, render-mirror, loopback capture, and manual observation remain
  distinct evidence layers. Clean lower-layer evidence does not prove physical
  endpoint output is pop-free.

## Current focus

- Keep single-case smoke, regression aggregate, loopback wrapper, analyzer, and
  evidence-bundle reports machine-readable and aligned with `docs/dev/harness.md`.
- Preserve report paths, log paths, process cleanup summaries, evidence layer,
  backend evidence, endpoint verification status, warnings, and inconclusive
  reasons.
- Prefer structured JSON/report parsing over fragile text-only assertions.

## Current priority order

1. Maintain `scripts/test-harness-reports.ps1 -SelfTest` as the fast schema and
   contract check.
2. Use latest-smoke and latest-regression validation when changing report-writing
   behavior.
3. Keep archive-retention lookup working so documented report/log paths remain
   reviewable after active retention moves old runs.
4. Keep evidence-only cases separate from gate cases in regression summaries.
5. Use evidence bundles for DeepSeek, MiMo, Claude Code, or human review without
   upgrading the underlying evidence layer.

## Current validation baseline

- For report-contract changes, run:

```powershell
scripts/test-harness-reports.ps1 -SelfTest
```

- When a smoke report exists or the task changes smoke report writing, also run:

```powershell
scripts/test-harness-reports.ps1 -LatestSmoke
```

- When changing regression aggregation, run a narrow regression case and then:

```powershell
scripts/test-harness-reports.ps1 -LatestRegression
```

## Current acceptance bar

- Reports use only `PASS`, `FAIL`, or `INCONCLUSIVE` as top-level results.
- Warning context is preserved without becoming a top-level result.
- Backend evidence records what was actually verified without upgrading lower
  evidence layers.
- Evidence-layer limitations are explicit in the report and in any final claim.
- Generated artifacts stay out of git unless explicitly requested.

## Dated notes

- 2026-05-19: Created this tracker. Fast report-contract validation currently
  passes with `scripts/test-harness-reports.ps1 -SelfTest`.
- 2026-05-21: Added ASIO selection fields to smoke harness reports. Latest ASIO
  selection/start smoke is schema-valid but `INCONCLUSIVE` pending
  ASIO-specific evidence semantics.
- 2026-05-21: Added v2 `backendEvidence` to smoke harness reports. ASIO
  selection/start now records driver-callback evidence separately from
  submitted-output and endpoint-output evidence.
- 2026-05-21: Switched old log/test-artifact retention from deletion to archive
  with `cache/test-artifact-archive/manifest.jsonl` lookup metadata.

## 状态刷新：2026-06-04（FFmpeg 静态链接修复 + 部署修复）

### 问题

- Smoke test 全部失败，exit code -1073741515 (STATUS_DLL_NOT_FOUND)
- 根因：FFmpeg 从 62.28 升级到 62.36 后，构建产物从静态库变为共享库
- `deploy_playable.cmake` 从未写过 FFmpeg DLL 复制逻辑
- 静态链接时不需要 DLL，动态链接后需要但没部署

### 修复

1. `scripts/build-ffmpeg-audio-core.ps1`：
   - `$commonFlags` 添加 `--disable-shared`（强制静态链接）
   - 源码目录不存在时自动 `git clone --depth 1 https://git.ffmpeg.org/ffmpeg.git`

2. `deploy_playable.cmake`：
   - PRUNE 逻辑从硬编码版本号（`avcodec-61.dll`）改为通配符匹配（`avcodec-*.dll`）
   - 避免 FFmpeg 版本升级后 PRUNE 失效

### 验证结果

- FFmpeg 构建：PASS（avcodec.lib = 2,659,886 字节，静态库）
- 应用构建：PASS
- 部署目录：无 av*.dll
- 单元测试：PASS（47/47）
- Smoke 测试：PASS（exit code 0, WASAPI shared）
- 测试日志：`build-mm\cache\logs\player-smoke-20260604-092929-070-b745de2a.harness.json`

### 教训

- FFmpeg 默认同时构建静态库和共享库，需要显式 `--disable-shared`
- `deploy_playable.cmake` 的 PRUNE 逻辑应该用通配符而非硬编码版本号
- 部署完整性验证应该纳入监督环节
