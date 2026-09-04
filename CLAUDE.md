# Project instructions

Canonical rules live in `AGENTS.md` and `docs/dev/*.md` — read those first for full detail.
Current issue/status index: `docs/bug/README.md`.
Current WASAPI investigation state: `docs/bug/wasapi-anomaly-status.md`.

## Current 0523-asio build contract
- Claude Code must not run Windows MSVC builds in this worktree by default. If
  the current Claude Code session only has Bash available, do not attempt a
  build, do not wrap `cmd.exe`, do not read old logs, and report:
  `未验证：当前 Claude Code Bash-only 执行环境无法可靠运行 Windows MSVC 构建。`
- For manual user validation, double-click the root script:

```cmd
build-mimo-asio.cmd
```

- For Codex/local or a confirmed working PowerShell terminal, command-line build
  validation is intentionally narrowed to one command:

```powershell
.\scripts\build-app-msvc.cmd -BuildDir build-mimo-asio -Configuration Debug
```

- A build is successful only when the same command output includes a fresh
  `buildLog:` path, that log contains
  `args=-BuildDir build-mimo-asio -Configuration Debug`, and the log ends with
  an `exe:` path under `build-mimo-asio\playable\Debug\...`.
- Do not use `build-mm`, `build-codex-asio*`, or older logs to judge the current
  0523-asio command-line build.
- Do not wrap the command in Bash. In VSCode's PowerShell terminal, run the
  PowerShell command above directly.
- VSCode CMake Tools is a separate IDE path that uses `build/` and the
  `windows-msvc-x64` preset. Do not mix VSCode `build/` results with
  command-line `build-mimo-asio` validation.
- These constraints are for Claude Code/manual build validation in this
  worktree. They do not change Codex/local's ability to use other checked-in
  scripts when a task explicitly requires them.

## General collaboration
- Make minimal, localized changes and keep the existing project structure.
- Prefer Qt official APIs and existing project patterns.
- Prefer checked-in PowerShell scripts under `scripts/` for build, smoke-test, log inspection, and playback automation.
- Commit only completed implementation work after validation passes; analysis-only tasks do not imply commits or pushes.
- Before finishing any file-changing task, follow the completion protocol in `docs/dev/agent-workflow.md`.

## Claude Code operating rules
- Reply in Chinese. Commands, paths, APIs, compiler diagnostics, and log excerpts may remain in their original language; plans, explanations, conclusions, and final summaries must be Chinese.
- Do not silently keep retrying when the environment looks broken. Stop and report before trying more workarounds if any of these happen:
  - the same build, environment, or tool failure occurs twice;
  - a command exits non-zero with no useful stdout/stderr;
  - stdout/stderr appears swallowed, a redirected log is not created, or a tool session looks abnormal;
  - the next step requires crossing Git Bash, cmd, PowerShell, MSVC, Qt, or FFmpeg environment boundaries;
  - the next step would change build scripts, CMake files, harness contracts, or take more than about five minutes.
- A blocked report must include the exact command tried, observed result, suspected blocking point, and recommended next step.
- For Windows MSVC app builds, prefer the native wrapper:
  `.\scripts\build-app-msvc.cmd -BuildDir <build-dir> -Configuration Debug`
  from PowerShell, or
  `cmd.exe /d /s /c ""scripts\build-app-msvc.cmd" -BuildDir <build-dir> -Configuration Debug"`
  from cmd-compatible launchers.
- Do not try to drive `cl.exe` from Git Bash by manually exporting `INCLUDE`, `LIB`, or Windows SDK paths. Use `scripts\build-app-msvc.cmd` or a real Visual Studio Developer Command Prompt.

## Work modes
- **ChatGPT/GitHub**: read-only analysis, planning, review, documentation drafts.
- **DeepSeek MCP sidecar**: quota-saving summaries, planning drafts, diff review, log triage, prompt compression — advisory only.
- **Codex/local**: primary agent — file edits, builds, smoke tests, log inspection, commits, pushes.
- **Claude Code**: co-primary agent alongside Codex/local. Same responsibilities (edits, builds, smoke tests, commits, pushes). Use separate branches to avoid conflicts — Codex on `codex-*`; Claude Code on the active main-model prefix, such as `MiMo-*`, `DeepSeek-*`, or `Claude-*`. Do not use a DeepSeek MCP sidecar call as the reason to switch to a `DeepSeek-*` branch. See `docs/dev/claude-code-workflow.md`.

## Playback architecture
- Preserve dual-worker playback model, teardown order, per-launch log rotation with archival retention, and structured log fields.
- Keep active WASAPI output switching separate from WASAPI error recovery.
- Route output mode, spatial-audio, device, and forced refresh changes through a unified transaction entry.
- Do not hide unclear switching structure with timing hacks, debounce hacks, longer fades, or silence padding.

## Diagnostics honesty
- Do not guess fixes for audible pops/clicks without improving observability.
- Decoder-side PCM, submitted backend PCM, and actual output-device audio are different evidence layers.
- Smoke tests must report `PASS`, `FAIL`, or `INCONCLUSIVE`.
- Do not claim audible pop/click fixes without diagnostic evidence and the stated limitation of the test layer.

## Automatic status landing
- When local Claude Code work changes behavior, fixes a bug, changes diagnostics or harness contracts, runs meaningful validation, produces new evidence, changes acceptance criteria, or discovers a new limitation, update the relevant `docs/bug/*-status.md` tracker without waiting for a separate user request.
- If the topic does not fit an existing tracker, update `docs/bug/README.md` and add a narrowly scoped tracker.
- If no tracker was updated, report why in the final response.
