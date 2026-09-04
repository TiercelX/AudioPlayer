# opencode workflow

Use this page when Xiaomi MiMo is connected to opencode for local agent work.
opencode is treated according to what it can actually do in the local
workspace:

- If opencode can read and edit files, run PowerShell or checked-in scripts,
  inspect logs and reports, and manage git state, it is a local implementation
  agent.
- If opencode only returns model text or remote advice without local execution,
  it is advisory only and cannot claim local validation.

For structure-split handoff, start with
`docs/dev/opencode-structure-prework.md`, then continue to the current quota
handoff and short-term task order in `docs/dev/opencode-four-day-roadmap.md`.

## Role boundary

Codex remains the default supervising agent in this repository unless the user
explicitly assigns a task to opencode. When opencode is assigned work, give it a
single focused task, exact allowed files or directories, forbidden files, a
dedicated worktree, a dedicated branch, and a dedicated build directory.

If Codex is unavailable and the user explicitly assigns opencode to run on its
own, opencode becomes the temporary master agent for that task window. In that
mode it owns task decomposition, validation, status landing, commit/push, and
the final handoff summary, but it must still follow this repository's scope and
evidence rules. It should prefer pushing completed task branches and an
opencode integration branch instead of pushing directly to `codex-*` branches
unless the user explicitly instructs it to update a Codex branch.

For master/subordinate opencode setups:

- The master agent owns task decomposition, file ownership, final diff review,
  final validation claims, status landing, and commit/push decisions.
- A subordinate agent works only inside the scope assigned by the master. It
  returns changed files, commands run, command results, generated evidence
  paths, and unresolved risks.
- Subordinates must not broaden scope, touch files owned by another active
  agent, or claim validation from commands they did not run or artifacts they
  did not inspect.
- If a subordinate cannot run local commands, treat its output like advisory
  review, not as local validation.

## Branch and worktree isolation

Use branch names that identify both the executor and the main model:

| Agent | Branch pattern | Example |
|-------|----------------|---------|
| Codex | `codex-MMDD-task` | `codex-0528-opencode-docs` |
| opencode with Xiaomi MiMo | `opencode-MiMo-MMDD-task` | `opencode-MiMo-0528-wasapi-plan` |
| opencode with another main model | `opencode-Model-MMDD-task` | `opencode-DeepSeek-0528-harness` |

- Use a separate `git worktree` for each active opencode task.
- Do not run opencode against Codex's active working directory unless the task
  is read-only.
- Do not edit the same file from Codex, Claude Code, and opencode in parallel.
- Use a task-specific build directory such as `build-opencode-wasapi` or
  `build-opencode-harness`.
- Commit and merge one completed branch at a time. Rebase or recreate dependent
  branches after merges.

For autonomous multi-day opencode runs:

- Create one integration branch from the current base, for example
  `opencode-MiMo-0528-four-day`.
- Create one task branch per day from the current integration branch, for
  example `opencode-MiMo-0528-day1-context-pack`.
- After a day's task validates, push the day branch, merge it into the
  integration branch locally, run the day's required checks again if the merge
  changed files, then push the integration branch.
- If a day fails validation, push the failed task branch only if it contains a
  useful failure note or diagnostic doc. Do not merge it into the integration
  branch.
- Keep `codex-*` branches untouched unless the user explicitly asks opencode to
  update them.

## Validation and claims

opencode may claim build, smoke-test, regression, log-inspection, report,
artifact, commit, and push results only when it actually performed that local
action or inspected the local artifact in its assigned worktree.

It must not claim:

- local build or smoke-test success from model reasoning alone;
- physical endpoint audio quality from internal PCM or submitted PCM checks;
- audible pop/click fixes without endpoint-layer diagnostic evidence and the
  stated limitation of that evidence;
- branch, commit, push, tag, release, or pull-request state unless it performed
  or inspected that git action locally.

Use the checked-in PowerShell scripts under `scripts/` for build, smoke-test,
report, evidence-bundle, and playback automation before ad-hoc commands.

## Completion

opencode follows the same completion protocol as Codex/local:

- inspect `git status` and classify every changed or untracked file;
- stage only intentional project changes;
- leave build outputs, logs, generated reports, and local-only files untracked
  unless the user explicitly asks to track them;
- update the relevant `docs/bug/*-status.md` tracker when the task changes
  behavior, diagnostics, harness contracts, validation confidence, or current
  investigation state;
- commit and push only completed validated work unless the user instructs
  otherwise.

## Local setup boundary

Do not add opencode tokens, Xiaomi MiMo credentials, local MCP gateway config,
shell profiles, or machine-specific opencode configuration to this repository.
Keep those files in the user's local tool configuration.
