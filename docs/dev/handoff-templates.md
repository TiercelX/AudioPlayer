# Handoff templates

Use this page when giving work to a Codex Luna subagent, DeepSeek MCP, manually
operated Claude Code, or opencode. Keep evidence claims honest: DeepSeek MCP is
advisory only, while Luna, Claude Code, and opencode can establish local
evidence only when they actually run commands or inspect local artifacts.

## Codex Sol-to-Luna execution prompt

Use this when Codex is running on GPT-5.6 Sol. Sol retains supervision and final
judgment; the GPT-5.6 Luna subagent performs the bounded local work.

```text
You are a GPT-5.6 Luna execution subagent for AudioPlayer. Work only in the
shared workspace and scope below. You are not the final decision maker.

Task:
<one bounded local execution task>

Reasoning effort:
- <low|medium|high|xhigh|max; state the selected value explicitly>
- Default to `medium`; use `low` for simple searches, read-only checks, or
  mechanical documentation work; use `high` for high-risk implementation,
  complex diagnosis, or independent review when deeper reasoning has practical
  value. Use `xhigh` or `max` only for an explicitly required or hardest
  quality-first task, or when representative validation shows a benefit.
- The actual GPT-5.6 Luna setting must match this field; do not rely on implicit
  inheritance.

Allowed files and commands:
- <exact files or directories>
- <checked-in scripts or narrow commands>

Do not touch:
- <files or areas owned by the supervising Sol agent or another active agent>
- unrelated user changes, local media, build outputs, logs, or generated reports
  unless the task explicitly requires them

Required context:
- AGENTS.md
- docs/dev/agent-workflow.md
- docs/dev/codex-workflow.md
- <narrow task-specific docs/status tracker>

Execution:
- Inspect current git status before editing.
- Use apply_patch for intentional file edits.
- Work through the task end to end where safe: search/read, implement, run the
  requested validation, inspect the diff and status, and prepare the handback.
- Avoid stopping after reconnaissance or handing the same exploration to another
  Luna; ask for a second Luna only for an independent read-only high-risk review.
- Run the smallest relevant checked-in validation.
- Do not broaden scope or overwrite unrelated changes.
- Do not commit or push unless Sol explicitly assigns that action.

Return to Sol:
- changed files
- exact commands run and results
- evidence/report paths inspected or generated
- remaining risks, ambiguities, and untouched user changes
```

## Prepare evidence

Prefer a compact bundle before asking for triage:

```powershell
scripts/collect-playback-evidence.ps1 -BuildDir build-mm
```

Pin a specific run when needed:

```powershell
scripts/collect-playback-evidence.ps1 `
  -BuildDir build-mm `
  -LogPath build-mm/cache/logs/player-smoke-YYYYMMDD-HHMMSS-fff-id.log `
  -LoopbackReportFile build-mm/cache/loopback/loopback-YYYYMMDD-HHMMSS-fff-id.report.json
```

Send the resulting `manifest.json`, `analyzer.report.json`, and, when present,
`loopback-alignment.report.json`. Add selected text-log or JSONL excerpts only
when the summary points to a specific event window.

## DeepSeek log triage prompt

Use `deepseek_log_triage` with v4-pro thinking for risky WASAPI/playback work.
Template:

```text
You are reviewing AudioPlayer harness evidence as an advisory sidecar only.
Do not claim local validation, commits, branch state, or endpoint audio quality.

Read these artifacts:
- manifest.json
- analyzer.report.json
- loopback-alignment.report.json if present
- selected log excerpts below

Task:
1. Summarize the top 3 observed facts.
2. Classify the evidence layer: internal PCM, submitted WASAPI PCM, loopback endpoint capture, or manual observation.
3. Identify whether the run is PASS, FAIL, or INCONCLUSIVE according to the harness contract.
4. List likely next files/scripts to inspect.
5. List conclusions that remain unproven.

Important constraints:
- Clean render-mirror or internal PCM does not prove speaker/headphone output.
- Loopback detector silence is still inconclusive when capture is interrupted or below physical endpoint evidence.
- Follow docs/dev/harness.md and docs/dev/deepseek-mcp-workflow.md.
```

## DeepSeek diff review prompt

Use `deepseek_diff_review` before committing risky script or WASAPI changes.
Template:

```text
Review this AudioPlayer diff as advisory only.
Focus on bugs, contract drift, missing validation, and evidence-layer mistakes.
Do not request broad refactors unless needed for correctness.

Project constraints:
- Smoke results must be PASS, FAIL, or INCONCLUSIVE.
- WARN must not be a top-level harness result.
- Submitted PCM evidence cannot prove endpoint pop/click absence.
- Harness changes should prefer structured JSON/report parsing over text-only assertions.

Output:
- Findings ordered by severity.
- Specific files/functions to inspect.
- Required local validation commands.
- Any conclusions that remain unproven until Codex/local or Claude Code runs validation.
```

## Claude Code implementation prompt

Use this when the user manually starts Claude Code with MiMo, DeepSeek, Claude,
or another main model. The user should create or resume a `MiMo-*`,
`DeepSeek-*`, or `Claude-*` branch before starting. For parallel local work,
use a separate `git worktree` and a unique task-specific branch.

```text
You are Claude Code operating as a co-primary local agent for AudioPlayer.
Use worktree <absolute path>.
Use branch <MiMo-MMDD-task, DeepSeek-MMDD-task, Claude-MMDD-task, or other active model prefix>.
Do not edit Codex's branch.

Task:
<one focused implementation task>

Scope:
- Allowed files: <list exact files or directories>
- Do not touch playback core unless explicitly listed.
- Do not edit the same files Codex is currently editing.
- Do not edit files assigned to another active local agent.

Required context:
- AGENTS.md
- docs/dev/agent-workflow.md
- docs/dev/harness.md
- docs/dev/claude-code-workflow.md
- Evidence bundle: <path to manifest.json>

Validation:
- Run the smallest relevant PowerShell validation first.
- Use build directory <build-task-name> for this task.
- For harness/report changes, run:
  scripts/test-harness-reports.ps1 -SelfTest
  scripts/test-harness-reports.ps1 -LatestSmoke
- If changing regression reporting, run a narrow case:
  scripts/run-playback-regression.ps1 -BuildDir build-mm -Configuration Debug -CaseFilter wav-play-stop -NoCleanup
  scripts/test-harness-reports.ps1 -LatestRegression

Completion:
- Report changed files.
- Report exact commands and results.
- Commit only completed validated work.
- Push the branch and give the commit hash.
- Do not claim audible pop/click fixes without endpoint-layer evidence and limitations.
```

## Codex-to-Claude CLI prompt

Use this when Codex invokes the local Claude Code CLI with `claude -p` instead
of asking the user to operate Claude Code manually. Claude's response is stdout
captured by Codex; it is not a live Codex subagent notification. Codex should
relay the useful output to the user when requested. When the user asks to learn
the interaction pattern, Codex should include the prompt it sent and the
important Claude output excerpts in its response. For implementation tasks,
Codex should create the separate worktree and branch before invoking Claude CLI.

Implementation template:

```text
You are Claude Code operating non-interactively for AudioPlayer.
Use worktree <absolute path>.
Use branch <Claude-MMDD-task, MiMo-MMDD-task, or other model-prefix branch>.
You are not alone in the codebase: do not revert edits made by others.
Do not commit or push unless this prompt explicitly allows it.

Task:
<one focused implementation task>

Allowed files:
- <exact file or directory list>

Do not touch:
- <exact forbidden files or areas>
- build outputs, logs, generated JSON/WAV/raw artifacts

Implementation constraints:
- Prefer moving existing code unchanged before refactoring.
- Keep public CLI parameters, report schemas, and playback behavior stable unless
  the task explicitly changes them.
- Stop and explain instead of broadening scope when the safe boundary is unclear.

Validation:
- Run <smallest relevant command>.
- Run `git diff -- <scope>` and `git status --short`.
- Do not claim build, smoke, endpoint, or audible validation unless you actually
  ran the command or inspected the artifact.

Completion response:
- changed files
- exact commands run and result
- what changed
- remaining risks
- concise output suitable for Codex to relay to the user when requested
```

Read-only review template:

```text
You are Claude Code doing a read-only review for AudioPlayer.
Do not edit files. Do not commit or push.

Review scope:
- <diff, files, reports, or evidence paths>

Focus:
1. Correctness risks.
2. Contract or evidence-layer drift.
3. Missing validation.
4. Overstated status or endpoint-audio claims.

Return findings ordered by severity, or say no issues found. Include specific
files/functions to inspect and validation required before landing. Keep the
response concise enough for Codex to quote or summarize to the user.
```

## opencode MiMo implementation prompt

Use this when the user starts opencode with Xiaomi MiMo as a local
implementation agent. Create or resume an `opencode-MiMo-*` branch in a
separate worktree before starting implementation work. For master/subordinate
setups, give each subordinate a smaller file and command scope than the master.

```text
You are opencode using Xiaomi MiMo as a local implementation agent for AudioPlayer.
Use worktree <absolute path>.
Use branch <opencode-MiMo-MMDD-task>.
Do not edit Codex's branch, Claude Code branches, or files owned by another active agent.

Task:
<one focused implementation task>

Role:
- <master or subordinate>
- If master: own task split, final diff review, validation claims, status landing, and commit/push decisions.
- If subordinate: only edit the allowed files below and return results to the master; do not commit or push unless explicitly allowed.

Scope:
- Allowed files: <list exact files or directories>
- Do not touch playback core unless explicitly listed.
- Do not edit generated logs, build outputs, media samples, or report artifacts unless explicitly asked.

Required context:
- AGENTS.md
- docs/dev/agent-workflow.md
- docs/dev/opencode-workflow.md
- docs/dev/harness.md
- Evidence bundle: <path to manifest.json, or "none">

Validation:
- Run the smallest relevant checked-in PowerShell validation first.
- Use build directory <build-opencode-task-name>.
- Do not claim build, smoke, endpoint, audible, commit, or push results unless you actually ran the command or inspected the local artifact.

Completion:
- Report changed files.
- Report exact commands and results.
- Report generated evidence paths.
- Report unresolved risks and files intentionally left untouched.
- Commit and push only if this prompt explicitly assigns that to you.
- Do not claim audible pop/click fixes without endpoint-layer evidence and limitations.
```

## Handoff back to Codex

Ask Claude Code or opencode to return:

- branch name and commit hash;
- exact changed files;
- exact validation commands and outputs;
- any generated evidence bundle path;
- unresolved risks or files intentionally left untouched.

Codex/local should then inspect the diff and relevant artifacts before merging or
building on the handoff.
