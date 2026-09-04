# ChatGPT GitHub workflow

Use this page to save Codex/local quota by doing read-only work in ChatGPT with
GitHub context, while keeping execution and validation claims honest.

## Use ChatGPT/GitHub for

- Reading committed code and documentation.
- Explaining architecture and control flow.
- Finding candidate files/functions for a change.
- Drafting implementation plans, review checklists, issue notes, PR summaries,
  and documentation edits.
- Reviewing diffs or proposed patches from GitHub context.

## Do not use ChatGPT/GitHub to claim

- A local build passed.
- A PowerShell script or smoke test passed.
- A generated log/report exists on the local machine.
- Actual speaker/headphone output is pop-free.
- A local branch, commit, push, or release exists.

Those claims require Codex/local workspace evidence or user-provided artifacts.

## Use Codex/local for

- Editing files in the workspace.
- Running `scripts/build-app.ps1`.
- Running `scripts/run-playback-smoke.ps1` or
  `scripts/run-playback-regression.ps1`.
- Inspecting generated text logs, JSONL diagnostics, reports, raw captures, and
  local untracked files.
- Creating commits, pushes, tags, or release artifacts.

## Push cadence

- After each stable completed change, Codex/local should commit and push the
  current collaboration branch unless the user explicitly says not to.
- For longer implementation sessions, Codex/local should avoid leaving validated
  work local-only for more than about 60-90 minutes. If the work is not yet
  committable, narrow to the next completed validated slice rather than pushing
  a partial or failing state.
- Push before asking ChatGPT/GitHub to review current code, otherwise that
  review may be based on stale committed context.
- This lets the user inspect the latest state from ChatGPT/GitHub and bring
  back review notes or prompts without spending Codex/local quota on read-only
  analysis.
- Analysis-only turns do not require commits or pushes.

## Good ChatGPT/GitHub prompts

- "Summarize the current WASAPI active output-switch transaction structure."
- "Find all code paths that write `report.result`."
- "Review this harness contract against `docs/dev/harness.md`."
- "Propose the smallest patch scope. Do not claim local test results."
- "Draft a Codex task with files to inspect and scripts to run."

## Handoff to Codex/local

When moving from ChatGPT/GitHub to Codex/local, include:

- target branch or commit if relevant;
- files/functions to inspect;
- intended behavior change;
- exact scripts to run;
- expected report/log fields;
- which conclusions require manual listening or loopback capture.

If ChatGPT/GitHub only produced a hypothesis, label it as a hypothesis until
Codex/local validation or user-provided evidence confirms it.
