# Project instructions

Durable rules for this Qt desktop app. Detailed guidance lives in `docs/dev/*.md`; issue/status tracking lives in `docs/bug/README.md`, with current WASAPI investigation state in `docs/bug/wasapi-anomaly-status.md`.

## General collaboration
- Make minimal, localized changes and keep the existing project structure unless there is a concrete reason to do otherwise.
- Explain the plan before editing code; keep plans and progress updates brief.
- Prefer Qt official APIs and existing project patterns.
- Do not rename files or classes unless required.
- Read and edit the smallest relevant set of files needed for the task.
- Prefer checked-in PowerShell scripts under `scripts/` for routine build, smoke-test, log inspection, and playback automation before ad-hoc commands.
- Run unit tests before committing code changes: `scripts\run-tests.ps1` (builds + runs all test suites via ctest). Use `-NoBuild` to skip rebuilding, `-Verbose` for detailed output.
- For full validation (unit tests + report schema + optional smoke): `scripts\validate-all.ps1`. This is the single command to verify project health before committing.
- Commit only completed implementation work after validation passes; analysis-only tasks do not imply commits or pushes.
- Before finishing any file-changing task, follow the completion protocol in `docs/dev/agent-workflow.md`.

## Progressive disclosure
- General workflow and scope: `docs/dev/agent-workflow.md`.
- Code/file navigation map: `docs/dev/code-map.md`.
- ChatGPT/GitHub read-only review flow: `docs/dev/chatgpt-github-workflow.md`.
- Codex Sol/Luna supervisor-executor workflow: `docs/dev/codex-workflow.md`.
- DeepSeek MCP sidecar workflow: `docs/dev/deepseek-mcp-workflow.md`.
- Claude Code co-primary workflow: `docs/dev/claude-code-workflow.md`.
- opencode local-agent workflow: `docs/dev/opencode-workflow.md`.
- DeepSeek/Claude Code/opencode handoff prompt templates: `docs/dev/handoff-templates.md`.
- Build, smoke-test, and playback automation: `docs/dev/harness.md`.
- Logging, reports, and evidence layers: `docs/dev/diagnostics.md`.
- Durable WASAPI switching rules: `docs/dev/wasapi-switching.md`.
- Commit, push, and release details: `docs/dev/release-workflow.md`.
- Linux ALSA backend development: `docs/dev/alsa-backend.md`.
- Linux development environment setup: `docs/dev/linux-dev-setup.md`.
- Bug/status tracking index: `docs/bug/README.md`.
- Current WASAPI anomaly scope and validation notes: `docs/bug/wasapi-anomaly-status.md`.
- Current ASIO backend status: `docs/bug/asio-status.md`.
- Current playback cache/source-preparation status: `docs/bug/playback-cache-status.md`.
- Current harness/report status: `docs/bug/harness-report-status.md`.
- Current Linux ALSA backend status: `docs/bug/alsa-status.md`.

## Work modes
- Use ChatGPT/GitHub for read-only analysis, planning, review, and documentation drafts.
- When Codex runs on GPT-5.6 Sol, keep Sol as the supervising agent and delegate
  bounded local execution to a GPT-5.6 Luna subagent by default. Local execution
  includes repository searches, file edits, builds, tests, log/report inspection,
  and other command-heavy work; target Luna for about 80% of that practical work
  (normally 75%-85%). Sol owns task framing, scope, final diff review, evidence
  judgment, status landing, and the final response. See
  `docs/dev/codex-workflow.md`.
- Set GPT-5.6 Luna's `reasoning_effort` explicitly in every Sol-to-Luna handoff; use `medium` by default.
- Use DeepSeek MCP as a sidecar for quota-saving summaries, planning drafts, diff review, log triage, and prompt compression when its output can be treated as advisory.
- Prefer DeepSeek v4-pro with thinking enabled for higher-strength sidecar review/planning/triage work, but keep Codex/local responsible for final edits and validation.
- Use Codex/local workspace for file edits, builds, smoke tests, log inspection, commits, and pushes.
- Use Claude Code as a co-primary agent alongside Codex/local. Claude Code may use MiMo, DeepSeek, Claude, or another model as its main model, while DeepSeek MCP remains available as an advisory sidecar. Co-primary agents have the same responsibilities (file edits, builds, smoke tests, log inspection, commits, pushes). Prevent conflicts by using separate branches: Codex on `codex-*`; Claude Code on a main-model branch prefix such as `MiMo-*`, `DeepSeek-*`, or `Claude-*`. See `docs/dev/claude-code-workflow.md`.
- Use opencode with Xiaomi MiMo as a local implementation agent when it can read/write the worktree and run local commands. Treat opencode output as advisory only if it cannot execute locally. Prevent conflicts with separate worktrees and branch names that include both executor and model, such as `opencode-MiMo-*`. See `docs/dev/opencode-workflow.md`.
- Do not claim local validation from GitHub-only context; see `docs/dev/chatgpt-github-workflow.md`.
- Do not claim local validation from DeepSeek-only output; see `docs/dev/deepseek-mcp-workflow.md`.

## Playback architecture invariants
- Preserve the current dual-worker playback model unless explicitly asked to refactor it.
- Preserve teardown order: release audio output resources, stop the decoder worker, then clear buffer/device resources.
- Treat existing playback threading and teardown behavior as intentional.
- Preserve per-launch log rotation: timestamp plus PID, with old logs archived after the active retention limit.
- Keep structured playback logs, including category and thread ID fields, unless logging requirements explicitly change.
- Keep large local media samples out of main git history unless explicitly asked.

## Durable WASAPI principles
- Keep active WASAPI output switching separate from WASAPI error recovery.
- Active output-switch transactions include output mode, spatial-audio, output device, and forced output configuration changes.
- Error-recovery transactions include WASAPI errors, device invalidation, and output resource failures.
- Route output mode, spatial-audio, device, and forced refresh changes through a unified high-level transaction entry in `WindowsWasapiAudioPlayer`.
- Keep buffer, session, and generation lifecycle boundaries explicit.
- Use hot reconfigure only when compatibility, recovery state, lifecycle boundaries, and stale-data safety are clear; otherwise prefer rebuild.
- Do not hide unclear switching structure with timing hacks, debounce hacks, longer fades, or silence padding.

## Durable ALSA principles
- 保持与 WASAPI 后端相同的双 Worker 架构
- 格式协商采用降级链路：hw: → plughw: → FFmpeg
- 错误恢复策略：XRUN → 设备挂起 → 设备断开 → 格式降级
- 设备枚举使用 snd_device_name_hint()
- hw: 设备独占，不支持多应用同时播放

## Diagnostics honesty
- Do not guess fixes for audible pops/clicks without improving observability.
- Treat decoder-side PCM, internal levels, submitted backend PCM, and actual output-device audio as different evidence layers.
- Internal PCM checks do not prove actual speaker/headphone output is pop-free.
- Smoke tests must report `PASS`, `FAIL`, or `INCONCLUSIVE`.
- Do not claim audible pop/click fixes without diagnostic evidence and the stated limitation of the test layer.

## Automatic status landing
- Do not wait for the user to ask for documentation updates when a task changes current project state.
- If a task adds or changes behavior, fixes a bug, changes diagnostics or harness contracts, runs meaningful validation, produces new evidence, changes acceptance criteria, or discovers a new limitation, update the relevant `docs/bug/*-status.md` tracker in the same completed work.
- If the topic does not fit an existing tracker, update `docs/bug/README.md` and add a narrowly scoped new tracker.
- Keep status notes factual: exact command/report/log paths when available, result limits, and next priority. Do not claim validation from advisory-only tools.
- If no status tracker was updated, say why in the final response.

## Current issue tracking
Use `docs/bug/README.md` as the index for current status trackers. Use `docs/bug/wasapi-anomaly-status.md` as the source of truth for WASAPI anomaly scope, validation notes, and next-step priorities.

## Linux 开发
- Linux ALSA 后端开发指南：`docs/dev/alsa-backend.md`
- Ubuntu 开发环境搭建：`docs/dev/linux-dev-setup.md`
- ALSA 后端状态跟踪：`docs/bug/alsa-status.md`
- 独占模式测试需要真实 Linux 环境，WSL 仅支持编译和基础功能测试
