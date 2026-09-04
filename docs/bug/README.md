# Bug and status tracking

This directory holds current, problem-specific investigation state. Keep durable
workflow rules in `AGENTS.md` and `docs/dev/*.md`; keep temporary status,
evidence limits, next steps, and dated notes here.

## Current trackers

| File | Scope |
| --- | --- |
| `wasapi-anomaly-status.md` | Windows WASAPI shared/exclusive/stability-mode behavior, output switching, recovery, submitted PCM, and endpoint-output anomaly evidence. |
| `asio-status.md` | Windows ASIO backend behavior, ASIO driver selection, driver lifecycle, playback validation, and ASIO-specific regressions. |
| `mimo-asio-handoff-20260524.md` | Point-in-time MiMo Claude Code handoff for the Creative/Sound Blaster ASIO recovery worktree and follow-up cleanup notes. |
| `playback-cache-status.md` | Playback source preparation, sidecar remux cache, cache settings UI, cache pruning, and local diagnostic/cache retention behavior. |
| `harness-report-status.md` | Smoke/regression/loopback harness reports, evidence bundles, report schema, result contracts, and automation reliability. |
| `bit-depth-precision-status.md` | Bit-depth precision output, source-bit-depth-first matching, noise shaping (2nd-order LNS), and 32→24/32→16 conversion roadmap. |
| `performance-optimization-status.md` | Render hot path, startup latency, source preparation, PRNG, ring buffer, caching, and async optimizations. |
| `code-quality-status.md` | Code quality analysis, technical debt, oversized files, code duplication, refactoring roadmap, and recent code deduplication + unit test framework introduction. |
| `alsa-status.md` | Linux ALSA 后端开发进度、设备枚举、格式协商、独占模式验证和已知限制。 |
| `media-foundation-status.md` | Windows Media Foundation、原始 Dolby sidecar 的压缩类型/样本探测，以及历史交叉证据。 |
| `eac3-joc-status.md` | E-AC-3/JOC/Atmos native decoder、metadata、scene/panner、SOFA/BRIR 和自研 stereo 路线的当前状态。 |

## Ownership rules

- Keep each status file scoped to its problem domain. Do not use the WASAPI
  anomaly file as the catch-all project changelog.
- If evidence crosses domains, link the related status file and state which
  layer owns the next local change.
- Put durable rules in `docs/dev/*.md`; put temporary investigation state here.
- Treat backend-specific conclusions as backend-specific unless logs or reports
  show the issue crosses a shared layer.
- Prefer creating a new status file when a topic has its own evidence,
  validation path, and likely file ownership.

## Automatic landing

All local implementation agents should update these trackers as part of the same
completed work that changes current project state. The user should not need to
ask separately.

Update the narrowest matching tracker when a task:

- changes behavior or user-visible workflow;
- fixes, reproduces, rejects, or narrows a bug;
- changes diagnostics, harness reports, evidence bundles, scripts, or acceptance
  criteria;
- runs meaningful validation that changes current confidence;
- discovers a new limitation, evidence gap, ownership boundary, or next priority.

If no existing tracker fits, add a new status file and link it in the table
above. If a task does not update status tracking, the final response should say
why.

Status notes must remain evidence-honest:

- local build, smoke, regression, loopback, report, commit, and push claims
  require local evidence from the agent making the claim;
- advisory-only DeepSeek MCP or ChatGPT/GitHub output can propose wording, but
  cannot by itself establish validation status;
- clean internal PCM or submitted PCM does not prove physical endpoint audio is
  pop-free.

## Status file shape

Use this shape for new trackers and for refreshing existing ones:

```markdown
# <Problem domain> status

This file tracks <scope>. Keep durable rules in `AGENTS.md` and `docs/dev/*.md`.

## Status refresh: YYYY-MM-DD

- Current known state.
- Evidence limits.
- What this file is not tracking.

## Current focus

- The next narrow diagnostic, implementation, or validation goal.

## Current priority order

1. First priority.
2. Second priority.

## Current validation baseline

- Commands or evidence needed before claiming a result.
- Known limitations of the evidence layer.

## Current acceptance bar

- What success means for this problem domain.

## Dated notes

- YYYY-MM-DD: Short note, evidence path, validation result, and remaining risk.
```

Keep the current sections near the top authoritative. Older dated notes are
history and should not override newer `docs/dev` rules or a newer status refresh.
