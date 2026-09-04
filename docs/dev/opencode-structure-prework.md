# opencode structure prework

Use this file before `docs/dev/opencode-four-day-roadmap.md` when handing
structure-split work to opencode with Xiaomi MiMo. This is a short prerequisite
task, not a replacement for the four-day roadmap.

## When to use this

Use this prework when the immediate goal is to shrink oversized code or harness
files without changing playback behavior. The preferred first target is the
smoke harness because it is lower risk than ASIO or WASAPI backend movement.

Skip this file only when the branch already contains a validated commit that:

- keeps `scripts/run-playback-smoke.ps1` as orchestration only;
- moves report construction and diagnostic-output printing into helper
  functions;
- passes the validation commands below.

## Branch and worktree

Use a separate opencode worktree. Do not run opencode edits in Codex's active
working directory.

- Branch: `opencode-MiMo-MMDD-smoke-prework`
- Build directory: reuse an existing validated Debug build only for smoke runs,
  or use `build-opencode-smoke-prework` if a fresh build is needed.

## Required reading

Read only these files first:

- `AGENTS.md`
- `docs/dev/agent-workflow.md`
- `docs/dev/opencode-workflow.md`
- `docs/dev/structure-split-plan.md`
- `docs/dev/opencode-structure-prework.md`
- `docs/bug/harness-report-status.md`

Do not read ASIO or WASAPI backend files for this prework.

## Task: finish smoke harness split

Goal: keep `scripts/run-playback-smoke.ps1` focused on parameter declaration,
dot-sourcing, top-level orchestration, cleanup, and final failure handling.

Allowed files:

- `scripts/run-playback-smoke.ps1`
- `scripts/playback-smoke-assertions.ps1`
- `scripts/playback-smoke-runner.ps1` only if process/path setup needs a
  small call-shape fix
- `scripts/playback-smoke-evidence.ps1` only if evidence parsing call shape
  needs a small fix
- `docs/bug/harness-report-status.md` if fresh validation evidence is produced

Do not touch:

- C++ source files
- CMake files
- playback backend files
- generated logs, reports, audio captures, build outputs, or local tool
  diagnostics

Implementation rules:

- Preserve every public parameter of `scripts/run-playback-smoke.ps1`.
- Preserve harness report field names and result semantics.
- Prefer moving code unchanged into helper functions before refactoring.
- Keep assertion/result normalization in `Invoke-SmokeAssertions`.
- Put report-writing assembly in a helper such as `Write-SmokeHarnessReport`.
- Put console diagnostic output in a helper such as
  `Write-SmokeDiagnosticOutput`.
- Leave cleanup and final `FAIL` throwing in `run-playback-smoke.ps1`.

## Validation

Run:

```powershell
scripts/test-harness-reports.ps1 -SelfTest
scripts/test-harness-reports.ps1 -LatestSmoke
```

If a latest smoke report is unavailable or stale, run the narrow WASAPI shared
smoke from `docs/dev/claude-next-structure-handoff.md` or the current harness
tracker, then run `scripts/test-harness-reports.ps1 -LatestSmoke` against that
build directory.

## Completion response

Return:

- changed files;
- exact validation commands and PASS/FAIL/INCONCLUSIVE result;
- harness report path if a fresh smoke was run;
- whether `docs/bug/harness-report-status.md` was updated, or why not;
- remaining untracked/generated files left out of the commit.

After this prework is validated and committed, continue with
`docs/dev/opencode-four-day-roadmap.md`.
