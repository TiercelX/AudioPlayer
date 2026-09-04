# Agent workflow

Use this page for collaboration and change-scope rules. Use
`docs/bug/README.md` as the status-tracking index, and keep current WASAPI
investigation state in `docs/bug/wasapi-anomaly-status.md`.
Use `docs/dev/code-map.md` for first-pass file navigation before opening large
source files.

## Collaboration

- This is a Qt desktop application.
- Make minimal, localized changes.
- Explain the plan before editing code.
- Keep plan and progress explanations brief unless the user asks for more detail.
- Prefer Qt official APIs and existing project patterns.
- Do not rename files or classes unless required.
- Read and edit the smallest relevant set of files needed for the task.
- When adding features, describe which files changed.

## Scope

- Documentation-only tasks must not modify playback code, C++ source files,
  CMake files, or scripts unless the user explicitly expands the scope.
- Prefer localized refactors that reduce oversized source files before adding
  more logic to them.
- When a source file grows large, prefer splitting it by responsibility into
  adjacent implementation files instead of expanding one monolithic file further.
- Preserve existing behavior unless the task explicitly asks for a behavior
  change.

## ChatGPT and GitHub mode

- Use ChatGPT/GitHub for read-only analysis, planning, review, and documentation
  drafts when local execution is not needed.
- In GitHub-only context, do not claim local build, smoke-test, log, report,
  commit, push, or release results.
- When handing work to Codex/local, name the files to inspect, the intended patch
  scope, and the scripts that should validate it.
- See `docs/dev/chatgpt-github-workflow.md` for the detailed handoff pattern.

## Codex Sol/Luna mode

- When the primary Codex model is GPT-5.6 Sol, use Sol as the supervisor and
  delegate bounded local execution to a GPT-5.6 Luna subagent by default.
- Luna should perform repository searches, file edits, command execution,
  builds, tests, and log/report inspection inside the scope assigned by Sol.
- For ordinary Sol-led work, target Luna for about 80% of practical local
  operations (normally 75%-85%), measured by searches, reads, edits, commands,
  validation, and report inspection rather than any quota accounting. Keep one
  continuous primary Luna on the task when possible; use a second Luna only as
  an independent read-only review for high-risk work.
- Set Luna's `reasoning_effort` explicitly in each handoff. Use `medium` by
  default; `low` is suitable for simple searches, read-only checks, and
  mechanical documentation work, while `high` is reserved for high-risk
  implementation, complex diagnosis, or independent review when deeper
  reasoning is expected to provide practical value. Do not use `xhigh` or
  `max` by default.
- Sol remains responsible for understanding the request, defining scope,
  resolving ambiguity, reviewing the final diff and evidence, deciding whether
  validation is sufficient, landing status, and writing the final response.
- Sol may execute directly when Luna is unavailable, the user forbids
  delegation, the operation cannot be safely isolated, or a trivial action
  would cost more to hand off than to complete. State the exception briefly.
- Do not let Sol and Luna edit the same files concurrently. Luna must return the
  files changed, exact commands and results, evidence paths, and unresolved
  risks before Sol accepts the work.
- See `docs/dev/codex-workflow.md` for the detailed task and evidence contract.

## DeepSeek MCP sidecar mode

- Use DeepSeek MCP for quota-saving sidecar summaries, planning drafts, diff
  review, log triage, and prompt compression when the output can be treated as
  advisory.
- Prefer the v4-pro thinking path for higher-strength sidecar review, planning,
  and triage work; keep lightweight summaries on the cheaper default path.
- Do not use DeepSeek MCP output to claim local build, smoke-test, log, report,
  commit, push, release, or manual listening results.
- Codex/local remains responsible for file edits, repository state, validation,
  and final judgment.
- See `docs/dev/deepseek-mcp-workflow.md` for the detailed tool-selection and
  evidence rules.

## Claude Code co-primary mode

- Claude Code operates as a co-primary agent with the same responsibilities as
  Codex/local: file edits, builds, smoke tests, log inspection, commits, and
  pushes. Claude Code may use MiMo, DeepSeek, Claude, or another model as its
  main model.
- DeepSeek MCP remains available as an advisory sidecar during Claude Code
  sessions. It is not deprecated, and its output still follows the DeepSeek MCP
  evidence rules.
- **Branch isolation**: Codex uses `codex-*` branches. Claude Code uses a branch
  prefix that matches its active main model, such as `MiMo-*`, `DeepSeek-*`, or
  `Claude-*`. Never operate on each other's branches.
- **Same-task handoff**: if one agent must continue the other's work, the first
  commits and pushes, then the second pulls and resumes on its own branch.
  Never edit the same file on two branches simultaneously.
- **Multiple concurrent tasks**: run each active local task in its own
  `git worktree`, use a unique task-specific branch name, assign non-overlapping
  files, and keep each task's build directory separate.
- Follow the same completion protocol and validation expectations regardless of
  which agent is active.
- MiMo-as-main-model work may claim local validation only from actual Claude
  Code/local execution. DeepSeek MCP output remains advisory and cannot claim
  local validation by itself.
- See `docs/dev/claude-code-workflow.md`.

## opencode local-agent mode

- Use opencode with Xiaomi MiMo as a local implementation agent when it can
  read and edit files, run local PowerShell commands, inspect logs/reports, and
  manage git state in an assigned worktree.
- Treat opencode output as advisory only when it cannot execute local commands
  or inspect local artifacts.
- **Branch isolation**: Codex uses `codex-*` branches. opencode branches should
  identify both executor and model, such as `opencode-MiMo-*`. Never operate on
  each other's branches.
- **Master/subordinate setups**: the master agent owns task split, final diff
  review, validation claims, status landing, and commit/push decisions.
  Subordinate agents work only inside the files and commands assigned to them.
- Use a separate `git worktree`, a unique task-specific branch, non-overlapping
  file ownership, and a distinct build directory for each active opencode task.
- opencode may claim local validation only from actual local execution or local
  artifact inspection in its assigned worktree.
- See `docs/dev/opencode-workflow.md`.

## Unit testing

- Unit tests live in `tests/` and use Qt Test with a custom `main()` runner.
- 10 test suites: TestPcmStreamFormat, TestPcmUtils, TestAudioUtils,
  TestVolumeControl, TestAudioBuffer, TestAudioPlayerFactory, TestWasapiStates,
  TestAlsaLogic, TestAsioFormats, TestIntegration.
- `test_main.cpp` manually calls `QTest::qExec()` for each suite and writes a
  trace file to `build-mm/test-trace.txt` (path set via `AUDIOPLAYER_BUILD_DIR`
  compile definition). QTest stdout cannot be captured by PowerShell; use the
  trace file for results.
- Run via `scripts\run-tests.ps1` which handles MSVC environment, Qt prefix
  resolution, Qt DLL PATH, cmake configure, build, and ctest execution.
- `scripts\run-tests.ps1` supports `-NoBuild` (skip building), `-Verbose`
  (show trace file content), `-Configuration` (build type), `-QtPrefix`
  (override Qt location), `-ReportFile` (custom JSON report path).
  Default JSON report: `build-mm/test-report.json`.
- For full project validation, use `scripts\validate-all.ps1` which runs unit
  tests, report schema self-test, and optionally smoke tests. Writes
  `validation-report.json` to the build directory.
- ctest is configured via `add_test(NAME AudioPlayerTests COMMAND AudioPlayerTests)`
  in CMakeLists.txt. The single test entry runs all suites.
- When adding a new test suite: create `tests/test_foo.{h,cpp}`, add to
  `add_executable(AudioPlayerTests ...)` in CMakeLists.txt, add
  `#include "test_foo.h"` and `RUN_SUITE(TestFoo)` in `test_main.cpp`.

## Analysis, validation, and commits

- Analysis-only tasks do not require commits or pushes.
- Commit only completed implementation work after relevant validation passes.
- Do not commit partial, exploratory, or failing intermediate states.
- Keep each commit scoped to one completed task or one clearly separable subtask.

## Status landing protocol

Agents should update status tracking as part of normal completion, not as a
separate user-driven chore. This applies to Codex/local and Claude Code when
they edit files, run local validation, inspect local logs/reports, or otherwise
produce new project state.

Update the relevant `docs/bug/*-status.md` tracker when the task:

- adds, removes, or changes user-visible behavior;
- fixes, reproduces, rejects, or narrows a bug;
- changes diagnostics, reports, evidence bundles, scripts, or harness contracts;
- runs meaningful build, smoke, regression, loopback, packaging, or manual
  validation that affects current confidence;
- discovers a new limitation, evidence gap, acceptance bar, or next priority;
- changes backend ownership boundaries, such as WASAPI versus ASIO versus shared
  source preparation.

When landing status:

- choose the narrowest matching tracker from `docs/bug/README.md`;
- add a dated note or refresh the current top sections when the new state should
  guide the next agent;
- include exact commands, report/log paths, result (`PASS`, `FAIL`, or
  `INCONCLUSIVE`), and evidence-layer limits when available;
- keep durable workflow rules in `docs/dev/*.md` instead of duplicating them in
  bug trackers;
- avoid rewriting long history unless the current status near the top would
  otherwise mislead the next agent.

If no existing tracker fits, add a new narrow tracker and link it from
`docs/bug/README.md`. If no status update is needed, the final response should
say why.

Advisory-only modes can draft status text, but they must not land claims that
require local evidence. Local agents must verify advisory findings before
committing them as project status.

## Mandatory completion protocol

Before finishing any task that changed files, run `git status` and classify every
changed or untracked path as one of:

1. intentional source/script/docs changes;
2. generated artifacts;
3. local-only files;
4. unknown.

Stage only intentional project changes. Do not stage `dist/`, `media/`, build
outputs, cache/logs, `token_count_report.csv`, or generated
`.wav`/`.json`/`.raw`/report artifacts unless the user explicitly asks to track
one of those outputs.

Commit and push completed intentional changes to the current tracking branch
unless the user explicitly says not to. Use the repository's normal validation
expectations before committing; documentation-only edits may use a scoped
documentation review instead of a build.

If anything is left uncommitted, list the exact files or directories and explain
why each was left out. The final response for file-changing tasks must include:

- commit hash, if a commit was made;
- pushed branch, if a push was made;
- status tracker updated, or why no tracker update was needed;
- remaining modified or untracked files;
- why each remaining file was left uncommitted.
