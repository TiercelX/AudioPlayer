# Claude next structure handoff

Use this file when continuing structure cleanup from Claude Code or a new Codex
thread. Current base branch: `codex-0416`, latest pushed state includes:

- `7a8888e` - smoke harness evidence parsing moved to
  `scripts/playback-smoke-evidence.ps1`.
- `f0c6605` - `src/ui/main.cpp` diagnostic report builder and
  `src/ui/mainwindow.cpp` responsibility splits landed.
- `20b9816` - rejected ASIO helper split attempt documented in
  `docs/bug/asio-status.md`.

## Current state

Completed and landed:

- `src/ui/diagnosticreportbuilder.h/.cpp`
- `src/ui/mainwindow_output.cpp`
- `src/ui/mainwindow_automation.cpp`
- `src/ui/mainwindow_media.cpp`
- `src/ui/mainwindow_cache.cpp`
- `src/ui/mainwindow_helpers.h/.cpp`
- `scripts/playback-smoke-evidence.ps1`

Do not redo those splits. If a branch still contains an unfinished
`Claude-0528-asio-helpers` attempt, treat it as disposable. That attempt failed
build validation, created unapproved `scripts/fix-corrupt.*` files, and was not
merged.

## Recommended next work

Prefer these tasks in order. Use one worktree and one branch per task.

### Task 1: finish smoke runner script split

Goal: reduce `scripts/run-playback-smoke.ps1` further without changing its CLI
contract or report schema.

Allowed files:

- `scripts/run-playback-smoke.ps1`
- `scripts/playback-smoke-assertions.ps1`
- `scripts/playback-smoke-runner.ps1`
- `scripts/playback-smoke-evidence.ps1` only for small call-shape fixes
- `docs/bug/harness-report-status.md` if validation evidence changes

Do not touch:

- C++ files
- CMake files
- playback backend files
- generated logs/reports/audio artifacts

Safe boundaries:

- Move assertion/result-normalization helper code to
  `scripts/playback-smoke-assertions.ps1`.
- Move pure path/process setup helpers to `scripts/playback-smoke-runner.ps1`.
- Keep top-level parameters, app launch orchestration, and final
  `Write-HarnessReport` call in `run-playback-smoke.ps1`.

Validation:

```powershell
scripts/test-harness-reports.ps1 -SelfTest
scripts/run-playback-smoke.ps1 -BuildDir build-structure-lowrisk -Configuration Debug -Source D:\AI\Codex\AudioPlayer\build-codex-asio-format-fallback\fixtures\sine-1khz-352800-stereo-s24.wav -QuitAfterMs 3000 -RequirePlaying -RejectPlaybackErrors
scripts/test-harness-reports.ps1 -LatestSmoke
```

Expected status landing:

- Update `docs/bug/harness-report-status.md` if a fresh smoke/report path is
  used or any report parsing/assertion behavior changes.

### Task 2: split CLI automation setup from `main.cpp`

Goal: move CLI option parsing and scheduled automation setup out of
`src/ui/main.cpp` while keeping `main()` as startup orchestration.

Allowed files:

- `src/ui/main.cpp`
- `src/ui/automationoptions.h`
- `src/ui/automationoptions.cpp`
- `CMakeLists.txt`
- `docs/bug/harness-report-status.md` if validation evidence changes

Do not touch:

- playback backends
- `MainWindow` split files unless a declaration dependency requires a tiny
  include-only change
- scripts

Validation:

```powershell
.\scripts\build-app-msvc.cmd -BuildDir build-structure-automation -Configuration Debug -FfmpegAudioCoreRoot D:\AI\Codex\AudioPlayer-codex-0523-asio\build-mm\ffmpeg-audio-core\runtime-with-ffprobe-msvc
scripts/run-playback-smoke.ps1 -BuildDir build-structure-automation -Configuration Debug -Source D:\AI\Codex\AudioPlayer\build-codex-asio-format-fallback\fixtures\sine-1khz-352800-stereo-s24.wav -QuitAfterMs 3000 -RequirePlaying -RejectPlaybackErrors
scripts/test-harness-reports.ps1 -LatestSmoke
```

Expected status landing:

- Update `docs/bug/harness-report-status.md` with exact build log and harness
  report paths, and state endpoint-output remains unverified unless loopback or
  manual evidence is collected.

## Defer for now

Do not start these unless explicitly requested and given a narrower prompt:

- ASIO helper extraction across multiple families in one pass.
- ASIO `AsioOutputWorker` extraction.
- WASAPI worker/header extraction.

For ASIO, the next safe attempt must be smaller than the rejected Slice D:

- one helper family only;
- no `AsioOutputWorker` edits;
- no temporary scripts;
- immediate Debug build before extracting another family.

For WASAPI, use a read-only planning/review pass first. Do not move render
callbacks, session/buffer generation checks, fade/gain state, or active-switch
guard code without a dedicated validation plan.

## Copyable Claude Code prompt

```text
You are Claude Code operating as a co-primary local agent for AudioPlayer.
Use worktree <absolute path>.
Use branch <Claude-MMDD-task or MiMo-MMDD-task>.
You are not alone in the codebase: do not revert edits made by others.
Do not commit or push unless explicitly told to.

Read first:
- AGENTS.md
- docs/dev/agent-workflow.md
- docs/dev/claude-code-workflow.md
- docs/dev/structure-split-plan.md
- docs/dev/claude-next-structure-handoff.md
- docs/dev/harness.md

Task:
<choose Task 1 or Task 2 from docs/dev/claude-next-structure-handoff.md>

Allowed files:
<copy the task's allowed files>

Do not touch:
<copy the task's forbidden files>

Rules:
- Preserve public CLI options, report schemas, playback behavior, file/class
  names, and existing project structure.
- Prefer moving code unchanged before refactoring.
- Stop and explain instead of broadening scope.
- Do not claim endpoint-output or audible validation without endpoint evidence.

Validation:
<copy the task's validation commands>

Completion response:
- changed files
- exact commands run and result
- what changed
- remaining risks
- concise output suitable for Codex/user relay
```

## Handoff back to Codex

Return:

- branch name and commit hash, if committed;
- exact changed files;
- exact validation commands and result lines;
- build log path and harness report path when available;
- any files intentionally left untouched;
- whether `docs/bug/harness-report-status.md` was updated, or why not.
