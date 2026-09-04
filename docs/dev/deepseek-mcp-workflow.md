# DeepSeek MCP workflow

Use this page to save Codex quota by delegating advisory sidecar work to the
local DeepSeek MCP server, while keeping repository edits and validation claims
honest.

## Purpose

DeepSeek MCP is a local Docker MCP sidecar. It is useful for analysis that can
be checked by Codex/local afterward:

- summarizing long context or logs;
- drafting implementation or investigation plans;
- reviewing small code snippets or diffs for likely risks;
- triaging test output or diagnostic logs;
- compressing long task history into a continuation brief.

It is not a replacement for Codex/local execution, project-specific judgment, or
manual audio evidence.

## Available tools

- `deepseek_chat`: general sidecar prompt. Default model is the lightweight
  `deepseek-v4-flash` path unless a model is specified.
- `deepseek_summarize`: cheap summaries with thinking disabled by default.
- `deepseek_review`: v4-pro code review with thinking enabled by default.
- `deepseek_plan`: v4-pro implementation or investigation plan draft.
- `deepseek_diff_review`: v4-pro diff review before commit or before a risky
  patch.
- `deepseek_log_triage`: v4-pro triage for build, test, smoke, or analyzer
  output.
- `deepseek_prompt_compress`: v4-pro continuation brief for long-running tasks.

For higher-strength advisory work, prefer `deepseek-v4-pro` with thinking
enabled and `reasoning_effort=high`. Use `reasoning_effort=max` only when the
extra cost is justified by ambiguity or risk.

## Use DeepSeek MCP for

- Read-only second opinions before editing.
- Plan drafts when the next local step is still unclear.
- Diff review after Codex/local has produced a candidate patch.
- Log triage when output is long and a concise failure hypothesis would help.
- Context compression before a restart, handoff, or long follow-up.

## Do not use DeepSeek MCP to claim

- A local build passed.
- A PowerShell script, smoke test, or playback regression passed.
- A generated local log, report, capture, or artifact exists.
- Actual speaker/headphone output is pop-free.
- A branch, commit, push, tag, release, or PR exists.
- A WASAPI or playback behavior change is correct without local evidence.

Those claims require Codex/local workspace evidence or user-provided artifacts.

## Evidence handling

- Treat DeepSeek output as advisory until checked against local files, logs,
  tests, or user-provided evidence.
- Label DeepSeek-only conclusions as hypotheses if they affect playback
  behavior, WASAPI switching, diagnostics, release readiness, or test verdicts.
- Do not paste secrets, private API keys, or large local media content into
  DeepSeek prompts.
- Prefer sending focused snippets, diffs, or log excerpts instead of broad
  repository dumps.
- If a DeepSeek recommendation conflicts with project instructions, follow
  `AGENTS.md` and the relevant `docs/dev/*.md` page.

## Handoff back to Codex/local

When using a DeepSeek result to guide local work, carry forward:

- which DeepSeek tool and model path was used;
- whether `reasoning_effort` was `high` or `max`;
- the hypothesis or recommendation;
- exact files/functions to inspect;
- validation scripts or manual checks needed;
- which conclusions remain unproven.

Codex/local should then inspect the relevant files directly, make any edits in
the workspace, and validate with the normal project scripts.

For reusable prompts, evidence-bundle inputs, and expected DeepSeek output
shape, see `docs/dev/handoff-templates.md`.

## Local setup boundary

The DeepSeek MCP server is configured outside this repository under the user's
local Codex tools directory. Do not add API keys, Docker MCP secrets, Codex MCP
gateway config, Claude MCP config, or local Docker profile snapshots to this
repository.
