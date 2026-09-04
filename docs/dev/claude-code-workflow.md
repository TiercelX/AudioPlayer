# Claude Code workflow

Codex and Claude Code are co-primary agents. Both have the same
responsibilities, regardless of whether Claude Code is backed by Claude, MiMo,
or another main model. The critical rule is: **separate branches, no shared
files in progress.**

DeepSeek MCP is still available during Claude Code sessions as an advisory
sidecar. It is not deprecated. Keep DeepSeek-as-Claude-Code-main-model separate
from DeepSeek MCP sidecar output: the former is co-primary local execution; the
latter is advisory only.

## Branch isolation

| Agent | Branch pattern | Example |
|-------|---------------|---------|
| Codex | `codex-MMDD` | `codex-0416` |
| Claude Code with MiMo main model | `MiMo-MMDD` | `MiMo-0518` |
| Claude Code with DeepSeek main model | `DeepSeek-MMDD` | `DeepSeek-0505` |
| Claude Code with Claude main model | `Claude-MMDD` | `Claude-0518` |

- Each agent works exclusively on its own branch.
- Never operate on the other agent's branch.
- Use the active main model name as the Claude Code branch prefix.
- Do not use a DeepSeek MCP sidecar call as the reason to switch to a
  `DeepSeek-*` branch; use `DeepSeek-*` only when DeepSeek is the Claude Code
  main model.
- Both push to the same remote; the user merges as needed.

## Starting a Claude Code session

- Create or resume a branch matching the active Claude Code main model, such as
  `MiMo-*`, `DeepSeek-*`, or `Claude-*`.
- `CLAUDE.md` loads project rules automatically.
- In the current `codex-0523-asio` worktree, command-line build validation is
  not a Claude Code responsibility by default. If the current Claude Code
  session only has Bash available, do not attempt Windows MSVC builds, do not
  wrap `cmd.exe`, and do not read older build logs as validation. Report:
  `未验证：当前 Claude Code Bash-only 执行环境无法可靠运行 Windows MSVC 构建。`
- For manual user validation, use the root click script:

```cmd
build-mimo-asio.cmd
```

- For Codex/local or a confirmed working PowerShell terminal, command-line
  validation is narrowed to:

```powershell
.\scripts\build-app-msvc.cmd -BuildDir build-mimo-asio -Configuration Debug
```

  Treat the build as successful only when this command emits a fresh
  `buildLog:`, that log records
  `args=-BuildDir build-mimo-asio -Configuration Debug`, and the same log ends
  with an `exe:` path under `build-mimo-asio\playable\Debug\...`. Do not use
  `build-mm`, `build-codex-asio*`, or older logs for this branch's current
  validation.
- Use the same PowerShell scripts under `scripts/` for build, smoke-test, log
  inspection, and playback automation.
- On Windows, if Claude Code's PowerShell tool or Git Bash-to-cmd output capture
  is unreliable, run the native wrapper instead:
  `.\scripts\build-app-msvc.cmd -BuildDir build-claude-asio -Configuration Debug`
  from PowerShell, or
  `cmd.exe /d /s /c ""scripts\build-app-msvc.cmd" -BuildDir build-claude-asio -Configuration Debug"`
  from cmd-compatible launchers.
  This wrapper imports the Visual Studio developer environment, adds common Qt
  CMake/Ninja paths, and writes a full build log under `build-claude-logs/`.
  Do not try to compile MSVC targets from Git Bash by exporting `INCLUDE` and
  `LIB`; launch through the wrapper or a real Visual Studio Developer Command
  Prompt.
- Follow the same completion protocol as Codex: classify every changed or
  untracked file with `git status`, stage only intentional changes, commit and
  push.

## Manual operator coordination

- In this project, assume Claude Code is manually operated by the user unless
  the user says otherwise.
- If the user says Codex may directly interact with Claude Code, Codex may use
  the local Claude Code CLI non-interactively as described below. Keep Codex
  responsible for final review, validation, status landing, commits, and pushes
  unless a task explicitly assigns those steps to Claude Code.
- Codex should not silently depend on Claude Code work happening in the
  background. When Claude Code help is useful, Codex should explicitly tell
  the user what to run or ask Claude Code to do, including the target branch,
  files, commands, and expected output.
- Prefer handing Claude Code a compact evidence bundle from
  `scripts/collect-playback-evidence.ps1` plus a focused task statement instead
  of a broad repository prompt.
- Use `docs/dev/handoff-templates.md` for copyable DeepSeek and Claude
  Code prompts.
- If both Codex and Claude Code may touch the same files, pause one side or
  finish, commit, and merge before starting the other side's edit.

## Multiple concurrent local agents

When running more than one Codex or Claude Code task at the same time, isolate
both repository state and generated outputs:

- Use a separate `git worktree` for each active task. Do not run multiple local
  agents against the same working directory.
- Use a unique branch name per task, not only per agent and date. Prefer names
  such as `codex-0520-harness`, `codex-0520-asio`, or
  `Claude-0520-wasapi-retry`.
- Give each task an explicit file or directory ownership list before it starts.
  Do not assign overlapping playback core files to two active tasks.
- Use a distinct build directory per task, such as `build-codex-harness` or
  `build-claude-wasapi`, so generated logs, reports, cache files, and build
  products do not overwrite each other.
- Land and merge one completed branch at a time. After a merge, rebase or
  recreate dependent task branches before continuing them.

## Codex-driven Claude CLI

Codex may invoke the local Claude Code CLI when `claude.exe` is available on
`PATH`, usually with non-interactive print mode:

```powershell
@'
<focused prompt>
'@ | claude -p --permission-mode acceptEdits --allowedTools "Read,Edit,Bash(git diff -- <scope>),Bash(git status --short),Bash(<validation command>)"
```

This is different from Codex built-in subagents. Built-in subagents return
Codex subagent notifications; Claude CLI output is ordinary stdout captured by
Codex. The user does not see that Claude CLI output live in a separate panel
unless they run Claude Code themselves. Codex should summarize the useful result
and, when the user asks to learn the interaction pattern, include both:

- the exact or lightly redacted prompt Codex sent to Claude; and
- the important Claude output excerpts, including findings, changed files,
  validation commands, and unresolved risks.

Keep these excerpts concise and do not paste large logs or full generated
artifacts unless the user explicitly asks for them.

Use two modes:

- **Advisory/read-only**: stay in the current worktree when Claude is only
  reviewing, drafting wording, or triaging. The prompt must say `Do not edit
  files. Do not commit or push.`
- **Implementation**: create a separate worktree and a `Claude-*`, `MiMo-*`, or
  other model-prefixed branch before invoking Claude CLI, then pass that
  absolute worktree path in the prompt. Give Claude exact allowed files,
  explicit forbidden files, validation commands, and whether it may commit or
  push. Codex must inspect the diff and rerun relevant validation before
  merging into its own branch.

When `claude -p` times out, check for leftover `claude` processes and stop only
the process Codex launched when it can be identified by PID or recent start
time. On Windows, use `Get-Process claude` to inspect candidates and
`Stop-Process -Id <pid>` only for the known launched process. Do not kill
unrelated user Claude Code sessions.

Prompt templates for Codex-driven Claude CLI calls live in
`docs/dev/handoff-templates.md`.

## Same-task handoff

When Codex runs out of quota mid-task, or when you want to switch agents for
another reason:

1. **From the current agent**: commit and push all completed work. Note the
   commit hash, active branch, current task scope, and last validation result.
2. **To the next agent**: pull the latest state. Create a new branch
   (`codex-MMDD`, `MiMo-MMDD`, `DeepSeek-MMDD`, or `Claude-MMDD`). The new
   branch can start from the old
   agent's tip commit.
3. **Never** have both agents editing the same file on different branches at
   the same time — this guarantees a merge conflict. If work targets the same
   files, wait for one agent to finish, merge, then start the other.

## What NOT to do

- Don't operate on the other agent's branch.
- Don't edit the same file in parallel on two branches.
- Don't leave uncommitted changes when handing off — the next agent can't
  see them.

## DeepSeek MCP sidecar

DeepSeek MCP tools remain available for advisory work during Claude Code
sessions. See `docs/dev/deepseek-mcp-workflow.md` for tool selection and
evidence rules. The reasoning_effort level should match the task risk: `high`
for normal review/triage, `max` only when ambiguity or risk justifies it.

Use Claude Code's effort setting for local main-agent work. DeepSeek MCP effort
remains configured separately by the MCP tool call.

## Claude Code effort selection

Use effort as a risk and ambiguity control, not as a default token-spending
knob. Higher effort is appropriate when extra reasoning reduces merge risk,
evidence mistakes, or rework; it is not needed for mechanical edits or narrow
summaries.

| Effort | Use for |
|--------|---------|
| `low` | Mechanical edits, formatting-only follow-ups, small grep/file summaries, and low-risk wording changes. |
| `medium` | Routine repository reading, handoff prompts, straightforward documentation updates, and small localized code edits with clear ownership. |
| `high` | Default for risky implementation and review work: WASAPI/ASIO behavior, playback lifecycle, threading, teardown, diagnostics contracts, harness/report behavior, and commit-time reviews. |
| `xhigh` | Cross-module root-cause analysis, competing design options, confusing logs plus source correlation, or review before touching shared playback boundaries. |
| `max` | Last-mile escalation for blocked or high-impact questions where a wrong conclusion would be expensive: persistent audible anomalies, hot-reconfigure versus rebuild boundaries, subtle stale-buffer/session-generation issues, or final red-team review of core audio-path changes. |

When dispatching Claude Code from Codex, prefer starting at the lowest effort
that fits the task, then escalate only when the first pass leaves unresolved
ambiguity. Large context windows make broad reading possible, but prompts should
still name the task, allowed files, evidence bundle, build directory, expected
output, and any files owned by another active agent.

## Evidence and claims

- Claude Code, like Codex, may claim local build passes, smoke-test results,
  log inspection findings, commits, and pushes.
- Claude Code must not claim audible pop/click fixes without diagnostic
  evidence and the stated limitation of the test layer.
- Main-model output from MiMo, DeepSeek, Claude, or another Claude Code model may
  claim local validation only when Claude Code actually ran the local command,
  inspected the local log, or produced the artifact in the workspace.
- DeepSeek MCP output during Claude Code sessions remains advisory; do not
  claim local validation from DeepSeek-only output.
