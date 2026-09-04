# Codex Sol/Luna workflow

Use this page when the primary Codex agent is running on GPT-5.6 Sol. The goal
is to reserve Sol for task understanding, engineering judgment, and acceptance
while using a GPT-5.6 Luna subagent for bounded local execution.

## Default role split

GPT-5.6 Sol is the supervising agent. It owns:

- understanding the user request and repository constraints;
- choosing the smallest safe scope and splitting work into bounded tasks;
- resolving ambiguity and deciding when user approval is required;
- reviewing Luna's diff, command results, logs, reports, and evidence limits;
- deciding whether validation is sufficient;
- updating or approving the relevant status tracker;
- commit/push decisions and the final response to the user.

GPT-5.6 Luna is the default execution subagent. Assign Luna:

- repository and code searches needed for the bounded task;
- file edits within explicitly allowed paths;
- checked-in build, test, smoke, playback, and diagnostic scripts;
- narrow ad-hoc commands when no checked-in script fits;
- log, report, artifact, and git-diff inspection;
- a structured handback of changed files, commands, results, evidence paths,
  and unresolved risks.

### Luna reasoning effort

Every Sol-to-Luna handoff must state the intended `reasoning_effort` explicitly,
and the actual subagent setting must match that handoff. Do not rely on implicit
inheritance. Use `medium` as the default for ordinary execution: it is the
balanced starting point for repository work, edits, validation, and handback.
Adjust the setting only when the task warrants it:

- `low` for simple searches, read-only checks, and mechanical documentation
  work;
- `high` for high-risk implementation, complex diagnosis, or independent review
  when deeper reasoning is expected to provide practical value;
- `xhigh` or `max` only when the task explicitly requires it, Sol judges it to
  be among the hardest quality-first tasks, or representative validation shows
  a meaningful benefit.

This policy changes only reasoning-depth selection; existing role ownership,
the practical 80% Luna target, and its risk-based exceptions remain unchanged.

The practical-work target for an ordinary Sol-led task is about 80% Luna, with
75%-85% as the normal operating range. Measure this by actual local work such
as tool calls, repository searches and reads, edits, builds/tests, and log or
report inspection. This is an operating target, not a promise about how the
Codex five-hour quota is calculated or restored.

Use these task-level targets as guidance, not hard success metrics:

- documentation, scripts, and small fixes: 85%-90% Luna;
- ordinary feature work: 75%-85% Luna;
- WASAPI, threading, lifecycle, or recovery work: 60%-75% Luna;
- diagnosis where the root cause is unclear: 50%-65% Luna.

Safety, clear ownership, and evidence take priority over reaching a percentage.

## Delegation rule

When the primary model is GPT-5.6 Sol, delegate local execution to GPT-5.6 Luna
by default before editing files or starting command-heavy work. Prefer one
continuous primary Luna that owns the bounded loop of search, implementation,
validation, diff/status inspection, and structured handback. Give the task one
concrete outcome, exact file or directory ownership, required context, allowed
validation commands, forbidden areas, and an explicit commit/push boundary.

For high-risk changes, Sol may add a second Luna for an independent read-only
review of the diff and evidence. Do not have multiple Lunas repeatedly explore
or edit the same files. Parallel Luna work is appropriate only when file
ownership and commands are clearly non-overlapping.

Sol may execute directly only when:

- GPT-5.6 Luna or subagent execution is unavailable;
- the user explicitly requests no delegation;
- the action cannot be safely isolated from Sol's active reasoning or another
  agent's file ownership;
- the action is trivial and the handoff overhead would exceed the work.

Sol should normally intervene at three checkpoints: task entry, the first
execution result, and final acceptance. Sol should not repeat broad searches,
editing, builds, or log analysis unless one of the exceptions above applies or
the returned evidence exposes a substantive ambiguity.

When using an exception, Sol should state it briefly in progress or the final
response. An exception does not relax repository scope, validation, evidence,
or completion rules.

## Shared-workspace safety

- Inspect `git status` before assigning edits and identify pre-existing user
  changes.
- Do not have Sol and Luna edit the same files concurrently.
- Use one writing agent per file set. Parallel Luna tasks must have
  non-overlapping file ownership and independent commands.
- Luna must not revert, stage, commit, or push changes outside its assigned
  scope.
- Sol must inspect the resulting diff and relevant raw command output or local
  artifacts before accepting Luna's conclusions.
- Prefer the prompt in `docs/dev/handoff-templates.md` for every execution
  handoff.

## Validation and evidence

A Luna subagent's local command can produce valid local evidence because it
runs in the shared workspace, but Sol must verify the returned output or
artifact before making the final claim. Neither model may claim:

- a build, test, smoke, regression, or report result that was not actually run;
- endpoint audio quality from internal or submitted PCM evidence alone;
- a commit, push, branch, tag, or release state that was not inspected locally;
- completion while Luna reports unresolved scope conflicts or unknown files.

Use the repository's mandatory completion protocol after Luna returns. Sol
remains accountable for classifying every changed path, status landing, final
validation judgment, and an evidence-honest user handoff.
