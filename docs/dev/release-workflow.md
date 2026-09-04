# Release workflow

Use this page for git, push, and release expectations.

## Git

- Do not rewrite git history or change version-control workflow unless the user
  explicitly asks for it.
- Analysis-only tasks do not require commits or pushes.
- Commit only completed implementation work after relevant validation passes.
- Do not commit partial, exploratory, or failing intermediate states.
- Keep each commit scoped to one completed task or one clearly separable subtask.
- Do not amend commits unless the user explicitly asks for it.
- After each stable completed change, push the current collaboration branch to
  GitHub unless the user explicitly says not to. This keeps ChatGPT/GitHub
  review context current without spending local Codex time on read-only review.
- Do not let validated implementation work sit only in the local workspace for
  long stretches. As a default cadence, create a scoped commit and push after
  each validated stable checkpoint; during long implementation sessions, aim to
  reach such a checkpoint and push at least every 60-90 minutes.
- If a task is still exploratory, failing validation, or behaviorally partial,
  do not make a misleading commit just to satisfy the clock. Instead narrow the
  scope to the next completed, validated slice that can be committed honestly.
- Prefer hyphenated Codex branch names such as `codex-0416` over slash-separated
  names such as `codex/0416` when the branch is expected to be used by GitHub or
  ChatGPT integrations.

## Pushes and releases

- Treat local commits, GitHub pushes, and GitHub Releases as separate steps.
- Push completed work at stable checkpoints after key validation passes, and at
  minimum by the end of a completed implementation turn.
- Also push before handing work to ChatGPT/GitHub for review, before switching
  to a substantially different task, and before any long pause where local-only
  changes would make GitHub context stale.
- Create GitHub Releases only for versions that are ready to be downloaded or
  tested.
- ChatGPT/GitHub can draft PR descriptions, release notes, and review
  checklists, but Codex/local or the user must perform final validation and git
  operations.
