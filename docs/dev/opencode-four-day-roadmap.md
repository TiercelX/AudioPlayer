# opencode four-day roadmap

Use this roadmap when Codex quota is unavailable and Xiaomi MiMo through
opencode needs to carry implementation forward. Current base branch:
`codex-0416`, latest pushed state includes:

- `4a90c23` - opencode MiMo workflow and prompt rules documented.
- `29b8972` - Codex adopted Claude structure splits and recorded validation.
- `5ccaa81` - smoke runner and assertions split landed.
- `294f5cb` - CLI automation options split landed.
- `20b9816` - broad ASIO helper split attempt rejected and documented.
- `b242d80` - smoke report writing and diagnostic output moved behind helper
  functions; opencode structure prework documented.

Before starting Day 1, complete or explicitly skip
`docs/dev/opencode-structure-prework.md`. That prework is the narrower
front-door for finishing the smoke harness structure split before moving into
ASIO/WASAPI mapping or backend work.

This is a context-saving plan first and an implementation plan second. Do not
start by reading whole 3000-line backend files. Use the file maps, status
trackers, and task scopes below to keep each opencode session small.

## Ten-hour day model

This roadmap assumes opencode/MiMo does the long local work and Codex is used
sparingly for setup, review, and merge decisions. If a one-hour interactive
Codex session effectively consumes about five hours of quota, do not spend
Codex time supervising every command. Use Codex to prepare the task packet,
then let opencode run the local task in its own worktree and return a compact
completion report.

Each 10-hour day should be planned as:

- 1 hour: task kickoff, reading only the required docs, branch/worktree setup.
- 4 hours: primary implementation or mapping work.
- 2 hours: build, smoke, report validation, and local artifact inspection.
- 1 hour: diff review, status landing, and commit/push.
- 2 hours: reserved fallback work from that day's safe backlog, or failure
  analysis if validation fails.

Do not use the reserve to start a second risky backend extraction in the same
branch. Use it for maps, prompt packs, validation notes, narrowly scoped helper
cleanup, or a smaller replacement task.

Daily completion bar:

- one validated commit, or one documented rejection with exact failure evidence;
- updated tracker only when implementation, diagnostics, validation confidence,
  or current bug state changed;
- no generated logs/reports/build outputs committed;
- a next-agent prompt or task card when the next day depends on the result.

## Autonomous opencode mode

Use this mode if Codex is unavailable for the whole four-day window and the user
wants opencode to keep working without interactive review.

Branch model:

- Base branch: current `origin/codex-0416` unless the user names a newer base.
- Integration branch: `opencode-MiMo-MMDD-four-day`.
- Day branches: `opencode-MiMo-MMDD-day1-context-pack`,
  `opencode-MiMo-MMDD-day2-asio-discovery`,
  `opencode-MiMo-MMDD-day3-asio-format-session`, and
  `opencode-MiMo-MMDD-day4-<chosen-track>`.

Daily autonomous loop:

1. Fetch and create or update the day's worktree from the current integration
   branch.
2. Read only the required docs for that day and the relevant status tracker.
3. Run the day's task. If validation fails, reduce scope once and retry. If it
   still fails, stop the implementation and write a failure note instead of
   broadening scope.
4. Run `git diff --check`, the day's validation commands, and `git status`.
5. Update the relevant `docs/bug/*-status.md` tracker only when the task changes
   behavior, diagnostics, harness contracts, validation confidence, or current
   bug state.
6. Commit only intentional source/script/docs changes. Leave logs, reports,
   build outputs, audio captures, and local diagnostics untracked.
7. Push the day branch.
8. If the day branch is validated, merge it into the integration branch and push
   the integration branch. If it is not validated, do not merge it.

Autonomous stop conditions:

- a build fails twice after narrowing the scope;
- a required smoke/regression command returns `FAIL`;
- a change would require editing files outside the day's allowed list;
- ASIO or WASAPI lifecycle boundaries become unclear;
- the task would require claiming endpoint audio behavior without endpoint
  evidence;
- unrelated user changes appear in the same files and cannot be separated
  safely.

End-of-window deliverable:

- pushed integration branch name and head commit;
- list of merged day branches and their commit hashes;
- list of failed or unmerged branches and why;
- exact validation commands and report/log paths for each completed day;
- status trackers updated, or why none were needed;
- remaining untracked/generated files intentionally left out;
- next recommended task and files to inspect first.

## Context-saving adjustments

Apply these rules before starting new implementation work:

- Start every opencode task from `AGENTS.md`,
  `docs/dev/opencode-workflow.md`, this roadmap, and the one relevant
  `docs/bug/*-status.md` tracker. Do not load all trackers.
- Use `docs/dev/code-map.md` to choose first files, then inspect only the
  functions needed for the task.
- Use one `git worktree`, one `opencode-MiMo-*` branch, and one build directory
  per task. Do not reuse Codex's active working directory for edits.
- Give subordinate opencode agents exact allowed files and forbidden files. A
  subordinate should return findings and patches to the master, not broaden the
  task.
- Prefer creating or updating narrow code-map documents before touching large
  audio files. A file map is valuable if it prevents repeated full-file reads.
- Keep status landing separate from durable workflow docs. Update a
  `docs/bug/*-status.md` tracker only when implementation, diagnostics,
  validation, or current confidence changes.
- Do not read or edit generated logs, JSON reports, WAV/raw captures, build
  outputs, or local tool diagnostics unless the task explicitly names them.
- Use evidence bundles from `scripts/collect-playback-evidence.ps1` for review
  and triage instead of pasting long logs.
- Preserve public CLI options, report schemas, playback behavior, class names,
  threading, teardown order, and WASAPI switching boundaries unless a task
  explicitly changes them.

Useful current size snapshot:

| File | Current size | Context guidance |
| --- | ---: | --- |
| `scripts/run-playback-smoke.ps1` | 254 lines | Already split. Treat as orchestration only. |
| `src/ui/main.cpp` | 24 lines | Already split. Do not spend time here. |
| `src/ui/mainwindow.cpp` | 674 lines | Already reduced. Only touch for focused UI fixes. |
| `src/backends/asio/windowsasioaudioplayer.cpp` | 3481 lines | Highest useful context-saving target, but split one helper family at a time. |
| `src/backends/wasapi/windowswasapiaudioplayer_worker.h` | 3332 lines | Defer implementation; do read-only mapping first. |

## Day 1: establish opencode handoff pack

Goal: make future opencode sessions cheaper before touching audio behavior.

Branch/worktree:

- Branch: `opencode-MiMo-0528-context-pack`
- Build directory: none unless a generated check is added

Allowed files:

- `docs/dev/opencode-four-day-roadmap.md`
- `docs/dev/asio-code-map.md`
- `docs/dev/wasapi-worker-map.md`
- `docs/dev/handoff-templates.md` only for a small prompt fix if needed

Do not touch:

- C++ source
- PowerShell scripts
- CMake files
- `docs/bug/*-status.md` unless an existing status note is clearly wrong

Tasks:

1. Create `docs/dev/asio-code-map.md` with a function/class index for
   `src/backends/asio/windowsasioaudioplayer.cpp`. Group entries into driver
   discovery, sample-rate/format negotiation, session probing, `AsioOutputWorker`,
   and `WindowsAsioAudioPlayer` lifecycle.
2. Create `docs/dev/wasapi-worker-map.md` with a read-only map of
   `src/backends/wasapi/windowswasapiaudioplayer_worker.h`. Group entries into
   output configuration, render loop, first-data/fade guards, session/buffer
   generation, diagnostics, and resource release.
3. Add one compact opencode prompt snippet to `docs/dev/handoff-templates.md`
   only if the existing template is missing a practical field needed by the
   maps.

10-hour supplement:

- Build the ASIO map as a function index first, then add a short dependency
  note for each extraction candidate: pure helper, reads player state, touches
  worker state, or lifecycle-sensitive.
- Build the WASAPI map as a risk map, not a refactor plan. Mark the functions
  that must stay together until endpoint or loopback evidence exists.
- Add a `Day 2 ready` checklist to the ASIO map: exact helper names to move,
  expected new file, CMake change, and validation commands.
- If the maps finish early, draft the opencode prompt for Day 2 in
  `docs/dev/handoff-templates.md`; do not start C++ edits on Day 1.
- If the maps take too long, finish ASIO first and create a small TODO section
  for WASAPI instead of reading the full WASAPI worker repeatedly.

Validation:

```powershell
git diff --check -- docs/dev/asio-code-map.md docs/dev/wasapi-worker-map.md docs/dev/handoff-templates.md
```

Expected completion:

- Commit and push the docs-only branch if the maps are accurate.
- No build required.
- No bug tracker update required unless a tracker correction was made.

## Day 2: ASIO discovery-only extraction

Goal: reduce ASIO context cost without touching worker lifecycle.

Branch/worktree:

- Branch: `opencode-MiMo-0529-asio-discovery`
- Build directory: `build-opencode-asio-discovery`

Allowed files:

- `src/backends/asio/windowsasioaudioplayer.cpp`
- `src/backends/asio/windowsasioaudioplayer_discovery.cpp`
- `CMakeLists.txt`
- `docs/dev/asio-code-map.md`
- `docs/bug/asio-status.md`

Do not touch:

- `AsioOutputWorker` implementation
- `WindowsAsioAudioPlayer` control-flow methods
- WASAPI files
- UI files
- harness scripts
- temporary repair scripts

Tasks:

1. Move only pure discovery helpers: COM registry enumeration, CLSID parsing,
   driver-name collection, and host-window discovery if currently independent
   of lifecycle state.
2. Keep helper names and behavior stable where possible. Prefer move-only edits.
3. Stop after this one helper family. Do not continue into format negotiation
   in the same branch.

10-hour supplement:

- Spend the first pass proving the boundary from `docs/dev/asio-code-map.md`.
  If any candidate helper touches `AsioOutputWorker`, decoder lifecycle, or
  recovery state, leave it in place and record why.
- After the first successful Debug build, run `git diff --check`, inspect the
  diff for accidental formatting churn, and only then run the ASIO smoke.
- If ASIO hardware is unavailable, run the build and add a status note that ASIO
  runtime behavior is unverified; use the reserve time to create a smaller Day 3
  task card rather than guessing.
- If the discovery split completes early, update the ASIO map with the new file
  ownership and prepare Day 3's exact allowed helper list. Do not move format
  helpers in the Day 2 branch.
- If the build fails, revert only the Day 2 branch's own edits or create a
  smaller patch that moves one helper at a time. Do not carry a failing broad
  split forward.

Validation:

```powershell
.\scripts\build-app-msvc.cmd -BuildDir build-opencode-asio-discovery -Configuration Debug -FfmpegAudioCoreRoot D:\AI\Codex\AudioPlayer-codex-0523-asio\build-mm\ffmpeg-audio-core\runtime-with-ffprobe-msvc
```

If a local ASIO device is available and the build passes, run one narrow ASIO
smoke with the known Creative index:

```powershell
scripts\run-playback-smoke.ps1 -BuildDir build-opencode-asio-discovery -Configuration Debug -Source D:\AI\Codex\AudioPlayer\build-codex-asio-format-fallback\fixtures\sine-1khz-352800-stereo-s24.wav -AsioOutputIndex 0 -QuitAfterMs 18000 -RequirePlaying
scripts\test-harness-reports.ps1 -LatestSmoke -BuildDir build-opencode-asio-discovery
```

Expected status landing:

- Update `docs/bug/asio-status.md` with exact build log and smoke report paths.
- If no ASIO smoke is run, say so and keep endpoint behavior unverified.

## Day 3: ASIO format/session helper extraction

Goal: continue ASIO shrink only if Day 2 built cleanly and was pushed.

Branch/worktree:

- Branch: `opencode-MiMo-0530-asio-format-session`
- Build directory: `build-opencode-asio-format`

Allowed files:

- `src/backends/asio/windowsasioaudioplayer.cpp`
- `src/backends/asio/windowsasioaudioplayer_formats.cpp`
- `src/backends/asio/windowsasioaudioplayer_sessionprobe.cpp`
- `CMakeLists.txt`
- `docs/dev/asio-code-map.md`
- `docs/bug/asio-status.md`

Do not touch:

- `AsioOutputWorker` implementation
- ASIO callback threading
- decoder-worker lifecycle
- WASAPI files
- harness scripts

Tasks:

1. If Day 2 failed or remained unmerged, do not implement this day. Instead,
   write a short failure analysis and a smaller replacement task.
2. Move only sample-type, channel-copy, sample-rate candidate, and PCM format
   helpers that are pure or near-pure.
3. Move external WASAPI session probing only if it is already independent of
   `AsioOutputWorker` internals.
4. Build immediately after each helper family if opencode can do incremental
   local validation.

10-hour supplement:

- Treat sample-format helpers and external session probing as two separate
  commits only if both build independently. If the first helper family consumes
  the day, stop there.
- Run one Debug build after each moved helper family before editing the next
  family. A later all-at-once failure is too expensive to untangle.
- If ASIO smoke passes, inspect the harness report for backend evidence fields:
  loaded backend, selected ASIO index, configure output observed, sample rate,
  buffer size, and first buffer switch.
- If time remains after validation, add a short `next safe ASIO split` section
  to `docs/dev/asio-code-map.md`. The next split should usually be worker
  mapping, not worker movement.
- If any validation is `INCONCLUSIVE`, preserve that wording in
  `docs/bug/asio-status.md` and state the evidence layer.

Validation:

```powershell
.\scripts\build-app-msvc.cmd -BuildDir build-opencode-asio-format -Configuration Debug -FfmpegAudioCoreRoot D:\AI\Codex\AudioPlayer-codex-0523-asio\build-mm\ffmpeg-audio-core\runtime-with-ffprobe-msvc
scripts\run-playback-smoke.ps1 -BuildDir build-opencode-asio-format -Configuration Debug -Source D:\AI\Codex\AudioPlayer\build-codex-asio-format-fallback\fixtures\sine-1khz-352800-stereo-s24.wav -AsioOutputIndex 0 -QuitAfterMs 18000 -RequirePlaying -RequireLogPattern 'asio sampleRate'
scripts\test-harness-reports.ps1 -LatestSmoke -BuildDir build-opencode-asio-format
```

Expected status landing:

- Update `docs/bug/asio-status.md`.
- Keep result `INCONCLUSIVE` for endpoint audio unless loopback/manual evidence
  is collected.

## Day 4: WASAPI read-only plan or low-risk harness polish

Choose only one track.

### Track A: WASAPI read-only map review

Use this if ASIO work exposed playback lifecycle uncertainty or if the next
likely work is WASAPI switching.

Branch/worktree:

- Branch: `opencode-MiMo-0531-wasapi-map-review`
- Build directory: none

Allowed files:

- `docs/dev/wasapi-worker-map.md`
- `docs/dev/wasapi-switching.md`
- `docs/bug/wasapi-anomaly-status.md` only if a status correction is needed

Do not touch:

- WASAPI C++ files
- ASIO files
- scripts
- CMake files

Tasks:

1. Review the WASAPI worker map against durable switching rules.
2. Identify the smallest future split that does not separate render callback,
   output configuration, first-data guards, fade/gain state, and
   session/buffer generation checks prematurely.
3. Produce a one-page future task card. Do not implement it.

10-hour supplement:

- Spend the reserve time turning the future task card into two alternatives:
  one read-only validation/mapping task and one implementation task. The
  implementation task must name exact functions and validation commands.
- Cross-check the task card against `docs/dev/wasapi-switching.md`; active
  switching and error recovery must remain separate.
- If a status correction is needed, keep it factual and cite exact files or
  commands. Do not claim audible output behavior from internal PCM evidence.
- If there is still time, write a handoff prompt for a future WASAPI-only
  branch. Do not edit WASAPI C++ on this day.

Validation:

```powershell
git diff --check -- docs/dev/wasapi-worker-map.md docs/dev/wasapi-switching.md docs/bug/wasapi-anomaly-status.md
```

Expected status landing:

- No bug tracker update unless the review changes current WASAPI priorities or
  corrects stale status.

### Track B: low-risk harness/report polish

Use this if opencode needs an implementation task but ASIO is not ready for
more changes.

Branch/worktree:

- Branch: `opencode-MiMo-0531-harness-polish`
- Build directory: `build-opencode-harness-polish`

Allowed files:

- `scripts/run-playback-smoke.ps1`
- `scripts/playback-smoke-runner.ps1`
- `scripts/playback-smoke-assertions.ps1`
- `scripts/playback-smoke-evidence.ps1`
- `docs/bug/harness-report-status.md`

Do not touch:

- C++ source
- CMake files
- playback backends

Tasks:

1. Inspect the already split smoke scripts for duplicated parsing or
   report-shaping code that can be moved without changing public CLI behavior.
2. Do not add new report fields unless the task explicitly requires them.
3. Keep `scripts/run-playback-smoke.ps1` as the entry point.

10-hour supplement:

- Use this track for mechanical cleanup only: helper naming, duplicated report
  shaping, narrow parser tests, or clearer validation output.
- If a fresh smoke is run, record the report path in
  `docs/bug/harness-report-status.md` and keep endpoint-output verification
  `INCONCLUSIVE` unless endpoint evidence exists.
- If the script is already sufficiently small, use the reserve time to improve
  handoff prompts or code maps instead of moving more code.
- Do not add new smoke parameters, report schema fields, or backend assertions
  as part of polish.

Validation:

```powershell
scripts\test-harness-reports.ps1 -SelfTest
scripts\run-playback-smoke.ps1 -BuildDir build-opencode-harness-polish -Configuration Debug -Source D:\AI\Codex\AudioPlayer\build-codex-asio-format-fallback\fixtures\sine-1khz-352800-stereo-s24.wav -QuitAfterMs 3000 -RequirePlaying -RejectPlaybackErrors
scripts\test-harness-reports.ps1 -LatestSmoke -BuildDir build-opencode-harness-polish
```

Expected status landing:

- Update `docs/bug/harness-report-status.md` if a fresh smoke/report path is
  used or if any report/assertion behavior changes.

### Track C: post-ASIO consolidation

Use this if Day 2 and Day 3 both completed cleanly and ASIO is the next likely
area of work.

Branch/worktree:

- Branch: `opencode-MiMo-0531-asio-next-card`
- Build directory: none unless a small generated check is added

Allowed files:

- `docs/dev/asio-code-map.md`
- `docs/dev/opencode-four-day-roadmap.md`
- `docs/dev/handoff-templates.md`
- `docs/bug/asio-status.md` only if correcting or summarizing validated state

Do not touch:

- C++ source
- CMake files
- scripts
- WASAPI files

Tasks:

1. Update the ASIO map to reflect the new ownership after Day 2/Day 3.
2. Identify the next smallest ASIO split. Prefer read-only worker mapping before
   moving `AsioOutputWorker`.
3. Draft a copyable prompt for the next opencode branch with exact allowed files,
   forbidden files, validation commands, and stop conditions.

Validation:

```powershell
git diff --check -- docs/dev/asio-code-map.md docs/dev/opencode-four-day-roadmap.md docs/dev/handoff-templates.md docs/bug/asio-status.md
```

Expected status landing:

- No tracker update unless the status summary changes current ASIO confidence or
  corrects stale validated evidence.

## Master prompt for opencode

```text
You are opencode using Xiaomi MiMo as a local implementation agent for AudioPlayer.
Use branch <opencode-MiMo-MMDD-task> in a separate git worktree.
Do not edit Codex's working directory or another agent's branch.

Read first:
- AGENTS.md
- docs/dev/opencode-workflow.md
- docs/dev/opencode-four-day-roadmap.md
- docs/dev/code-map.md
- the one docs/bug/*-status.md tracker named by the selected day

Task:
<copy exactly one day/task from docs/dev/opencode-four-day-roadmap.md>

Rules:
- Keep the task smaller than the roadmap if build validation fails.
- Preserve public CLI options, report schemas, playback behavior, class names,
  threading, teardown order, and WASAPI switching/recovery boundaries.
- Do not touch generated artifacts, local diagnostic logs, or large media files.
- Do not claim local validation unless you actually ran the command or inspected the artifact.

Validation:
<copy the selected day's validation commands>

Completion response:
- branch name and commit hash, if committed
- exact changed files
- exact commands and result lines
- build log path and harness report path when available
- status tracker updated, or why not
- unresolved risks and files intentionally left untouched
```

## Autonomous four-day prompt

Use this prompt when Codex will not be available to supervise the four-day run.

```text
You are opencode using Xiaomi MiMo as the temporary autonomous master agent for
AudioPlayer. Codex will not be available during this four-day window.

Base:
- Start from current origin/codex-0416 unless the user gives a newer base.
- Create integration branch opencode-MiMo-<MMDD>-four-day.
- Create one day branch per task from the current integration branch.
- Push each completed day branch.
- Merge only validated day branches into the integration branch, then push the
  integration branch.
- Do not push directly to codex-* branches unless the user explicitly asks.

Read first:
- AGENTS.md
- docs/dev/agent-workflow.md
- docs/dev/opencode-workflow.md
- docs/dev/opencode-structure-prework.md
- docs/dev/opencode-four-day-roadmap.md
- docs/dev/code-map.md

Execution:
- Complete or explicitly skip docs/dev/opencode-structure-prework.md first.
- Then execute Day 1 through Day 4 from docs/dev/opencode-four-day-roadmap.md.
- For each day, read only that day's required files and the one relevant
  docs/bug/*-status.md tracker.
- Use the checked-in PowerShell scripts for build, smoke, report, and evidence
  validation.
- If validation fails, narrow scope once. If it still fails, stop that branch,
  write a failure note, push the branch if useful, and do not merge it into the
  integration branch.

Rules:
- Preserve public CLI options, report schemas, playback behavior, class names,
  threading, teardown order, and WASAPI switching/recovery boundaries.
- Do not touch generated logs, JSON reports, WAV/raw captures, build outputs,
  large media files, or local tool diagnostics except to inspect named evidence.
- Do not claim endpoint audio quality without endpoint-layer evidence.
- Do not broaden a day's allowed files. If the task needs broader files, stop
  and write the next task card instead.
- Keep codex-* branches untouched.

Daily closeout:
- Run git diff --check for changed tracked files.
- Run the day's validation commands.
- Update the narrow docs/bug/*-status.md tracker only if implementation,
  diagnostics, validation confidence, or current status changed.
- Commit intentional changes only.
- Push the day branch.
- Merge into opencode-MiMo-<MMDD>-four-day only if validated.

Final response to user:
- integration branch and head commit;
- merged day branches and commit hashes;
- unmerged/failed branches and reasons;
- validation commands, PASS/FAIL/INCONCLUSIVE results, and report/log paths;
- status trackers updated or why not;
- remaining untracked/generated files intentionally left out;
- recommended next task.
```
