# Structure split plan

This is a practical plan for shrinking files that are expensive for
long-context agents to inspect. It is not an architecture rewrite. Preserve
existing behavior, file/class names, playback threading, teardown order, and
WASAPI switching/recovery boundaries while splitting responsibilities into
adjacent files.

## Priority order

Rank balances context cost, expected merge risk, and validation confidence.

| Priority | Current file | Size observed | Risk | Reason |
| --- | --- | ---: | --- | --- |
| 1 | `scripts/run-playback-smoke.ps1` | 1326 lines | Low to medium | Harness is oversized, mostly mechanical, and already has shared helpers. Split first to improve agent handoff without touching audio code. |
| 2 | `src/ui/main.cpp` | 1083 lines | Low to medium | Startup and diagnostic report logic are separable from app launch. This reduces report-context load before touching playback. |
| 3 | `src/ui/mainwindow.cpp` | 1580 lines | Medium | UI formatting, automation helpers, cache/media dialogs, and output menus can split without changing backend behavior. |
| 4 | `src/backends/asio/windowsasioaudioplayer.cpp` | 3481 lines | High | Large ASIO file mixes COM/driver discovery, worker implementation, and player lifecycle. Split only with ASIO validation and status landing. |
| 5 | `src/backends/wasapi/windowswasapiaudioplayer_worker.h` | 3332 lines | Highest | Largest reasoning burden, but it owns render callbacks, session/buffer boundaries, active switching guards, fade logic, and submitted-PCM diagnostics. Split last and narrowly. |

## Safe split boundaries

Future filenames are exact proposed targets. Do not rename existing classes.

### `scripts/run-playback-smoke.ps1`

Low-risk mechanical split. Keep the script as the command entry point and keep
parameter names stable.

- Move path, source, timeout, and process setup helpers to
  `scripts/playback-smoke-runner.ps1`.
- Move log/report extraction helpers and backend evidence shaping to
  `scripts/playback-smoke-evidence.ps1`.
- Move smoke assertions and result normalization glue to
  `scripts/playback-smoke-assertions.ps1`.
- Keep top-level orchestration, parameter validation, app launch, and final
  `Write-HarnessReport` call in `scripts/run-playback-smoke.ps1`.

Validation for any script split:

```powershell
scripts/test-harness-reports.ps1 -SelfTest
scripts/test-harness-reports.ps1 -LatestSmoke
```

If behavior changes or report fields move, also run the narrowest relevant
`scripts/run-playback-smoke.ps1` case and update
`docs/bug/harness-report-status.md`.

### `src/ui/main.cpp`

Low-risk C++ split. Keep `main()` in `src/ui/main.cpp`.

- Move diagnostic report helpers from the anonymous namespace to
  `src/ui/diagnosticreportbuilder.h` and
  `src/ui/diagnosticreportbuilder.cpp`.
- Move CLI automation option parsing and scheduled-action setup to
  `src/ui/automationoptions.h` and `src/ui/automationoptions.cpp`.
- Keep QApplication setup, log setup, player window construction, and event-loop
  ownership in `src/ui/main.cpp`.

Validation for any UI startup/report split:

```powershell
.\scripts\build-app-msvc.cmd -BuildDir build-structure-split -Configuration Debug
scripts/test-harness-reports.ps1 -LatestSmoke
```

If diagnostic report fields or CLI automation behavior changes, update
`docs/bug/harness-report-status.md`.

### `src/ui/mainwindow.cpp`

Mostly mechanical UI split. Keep the `MainWindow` class name and public API.

- Move output-device menu building, backend/device selection helpers, and WASAPI
  mode menu handling to `src/ui/mainwindow_output.cpp`.
- Move playback automation methods to `src/ui/mainwindow_automation.cpp`.
- Move media-info formatting and dialogs to `src/ui/mainwindow_media.cpp`.
- Move cache settings dialog handling to `src/ui/mainwindow_cache.cpp`.
- Keep constructor layout wiring, core signal connections, and basic
  play/pause/seek UI state in `src/ui/mainwindow.cpp`.

Validation for any main-window split:

```powershell
.\scripts\build-app-msvc.cmd -BuildDir build-structure-split -Configuration Debug
scripts/run-playback-smoke.ps1 -BuildDir build-structure-split -Configuration Debug
scripts/test-harness-reports.ps1 -LatestSmoke
```

If only code is moved and no behavior/report state changes, no bug tracker
update is needed. If automation, output selection, or cache behavior changes,
update the narrowest tracker: `harness-report-status.md`,
`wasapi-anomaly-status.md`, `asio-status.md`, or `playback-cache-status.md`.

### `src/backends/asio/windowsasioaudioplayer.cpp`

Risky audio lifecycle split. Do not mix this with WASAPI work, UI work, or
harness contract work.

- Move COM registry discovery, CLSID parsing, and host-window discovery helpers
  to `src/backends/asio/windowsasioaudioplayer_discovery.cpp`.
- Move ASIO sample type, PCM format, muxer/codec, and channel-copy helpers to
  `src/backends/asio/windowsasioaudioplayer_formats.cpp`.
- Move external WASAPI session probing used for ASIO busy/recovery decisions to
  `src/backends/asio/windowsasioaudioplayer_sessionprobe.cpp`.
- Move `AsioOutputWorker` implementation to
  `src/backends/asio/windowsasioaudioplayer_worker.cpp`.
- Keep `WindowsAsioAudioPlayer` public control flow, decoder-worker lifecycle,
  retry/recovery orchestration, and teardown sequence in
  `src/backends/asio/windowsasioaudioplayer.cpp`.

Validation for any ASIO split:

```powershell
.\scripts\build-app-msvc.cmd -BuildDir build-structure-asio -Configuration Debug
scripts/run-playback-smoke.ps1 -BuildDir build-structure-asio -Configuration Debug -AsioOutputIndex <known-local-index>
scripts/test-harness-reports.ps1 -LatestSmoke
```

Expected status landing: update `docs/bug/asio-status.md` with exact commands,
result, report/log paths, and evidence limits. Do not claim endpoint audio
quality unless endpoint evidence exists.

### `src/backends/wasapi/windowswasapiaudioplayer_worker.h`

Highest-risk split. Treat this as render-worker surgery, not cleanup. Keep
active output switching separate from WASAPI error recovery.

- Move non-member helper functions, format/channel helpers, environment parsing,
  and `safeRelease` to
  `src/backends/wasapi/windowswasapiaudioplayer_worker_helpers.h`.
- Move `WasapiOutputWorker` declarations to
  `src/backends/wasapi/windowswasapiaudioplayer_worker.h` and implementation to
  `src/backends/wasapi/windowswasapiaudioplayer_worker.cpp`.
- If the implementation remains too large, split only diagnostics-heavy helper
  methods to `src/backends/wasapi/windowswasapiaudioplayer_worker_diagnostics.cpp`.
- Keep render callback, `configureOutput`, `startOutput`, `releaseOutput`,
  `renderAvailableFrames`, first-data-block guards, fade/gain state, and
  session/buffer generation checks together until a dedicated validation pass
  proves the smaller split is behavior-preserving.

Validation for any WASAPI worker split:

```powershell
.\scripts\build-app-msvc.cmd -BuildDir build-structure-wasapi -Configuration Debug
scripts/run-playback-regression.ps1 -BuildDir build-structure-wasapi -Configuration Debug -CaseFilter wav-play-stop -NoCleanup
scripts/test-harness-reports.ps1 -LatestRegression
```

For active switching or spatial-output code movement, add the narrowest relevant
smoke/regression cases from `docs/dev/harness.md`. Expected status landing:
update `docs/bug/wasapi-anomaly-status.md` with exact commands, report/log
paths, result, and evidence-layer limits. Do not claim pop/click fixes without
manual or loopback endpoint evidence.

## Low-risk task slices

These are suitable for MiMo, Claude Code, or Codex with medium effort. Use a
separate worktree/branch per active agent and do not overlap files.

### Slice A: smoke runner helper extraction

- Objective: move pure helper/report extraction logic out of
  `scripts/run-playback-smoke.ps1` while preserving CLI behavior and report
  schema.
- Allowed files: `scripts/run-playback-smoke.ps1`,
  `scripts/playback-smoke-runner.ps1`,
  `scripts/playback-smoke-evidence.ps1`,
  `scripts/playback-smoke-assertions.ps1`,
  `docs/bug/harness-report-status.md`.
- Do not touch: C++ source, CMake files, playback backend files, generated
  reports/logs.
- Validation commands:

```powershell
scripts/test-harness-reports.ps1 -SelfTest
scripts/test-harness-reports.ps1 -LatestSmoke
```

- Expected evidence/status update: if only helpers move and report validation
  passes with existing latest report, note no tracker update is needed. If a
  fresh smoke run is required or report behavior changes, update
  `docs/bug/harness-report-status.md`.

### Slice B: diagnostic report builder extraction

- Objective: move log parsing and JSON report construction out of
  `src/ui/main.cpp`, keeping `main()` as orchestration.
- Allowed files: `src/ui/main.cpp`, `src/ui/diagnosticreportbuilder.h`,
  `src/ui/diagnosticreportbuilder.cpp`, CMake files only for adding the new
  source files, `docs/bug/harness-report-status.md`.
- Do not touch: playback backends, PowerShell scripts, UI layout, bug trackers
  outside harness unless evidence points there.
- Validation commands:

```powershell
.\scripts\build-app-msvc.cmd -BuildDir build-structure-report -Configuration Debug
scripts/test-harness-reports.ps1 -LatestSmoke
```

- Expected evidence/status update: update `docs/bug/harness-report-status.md`
  only if report fields, parsing behavior, or validation confidence changes.

### Slice C: main-window UI responsibility split

- Objective: move UI-only helper groups into adjacent `mainwindow_*.cpp` files
  without changing user-visible behavior.
- Allowed files: `src/ui/mainwindow.cpp`, `src/ui/mainwindow_output.cpp`,
  `src/ui/mainwindow_automation.cpp`, `src/ui/mainwindow_media.cpp`,
  `src/ui/mainwindow_cache.cpp`, CMake files only for adding new sources.
- Do not touch: playback backends, smoke scripts, report contracts.
- Validation commands:

```powershell
.\scripts\build-app-msvc.cmd -BuildDir build-structure-ui -Configuration Debug
scripts/run-playback-smoke.ps1 -BuildDir build-structure-ui -Configuration Debug
scripts/test-harness-reports.ps1 -LatestSmoke
```

- Expected evidence/status update: no tracker update for pure movement. Update
  a narrow tracker only if behavior or evidence changes.

## Audio lifecycle task slices

These require high effort and must not run in parallel with other work touching
the same backend.

### Slice D: ASIO helper extraction before worker movement

- Objective: move non-lifecycle ASIO discovery/format/session-probe helpers
  first, leaving `AsioOutputWorker` and `WindowsAsioAudioPlayer` lifecycle in
  place.
- Allowed files: `src/backends/asio/windowsasioaudioplayer.cpp`,
  `src/backends/asio/windowsasioaudioplayer_discovery.cpp`,
  `src/backends/asio/windowsasioaudioplayer_formats.cpp`,
  `src/backends/asio/windowsasioaudioplayer_sessionprobe.cpp`, CMake files only
  for adding new sources, `docs/bug/asio-status.md`.
- Do not touch: WASAPI files, smoke scripts, UI files, decoder/source-prep
  files.
- Validation commands:

```powershell
.\scripts\build-app-msvc.cmd -BuildDir build-structure-asio -Configuration Debug
scripts/run-playback-smoke.ps1 -BuildDir build-structure-asio -Configuration Debug -AsioOutputIndex <known-local-index>
scripts/test-harness-reports.ps1 -LatestSmoke
```

- Expected evidence/status update: update `docs/bug/asio-status.md` with exact
  command/report/log paths and note submitted-output and endpoint-output limits.

### Slice E: ASIO worker extraction

- Objective: move `AsioOutputWorker` into
  `src/backends/asio/windowsasioaudioplayer_worker.cpp` after Slice D is stable.
- Allowed files: `src/backends/asio/windowsasioaudioplayer.cpp`,
  `src/backends/asio/windowsasioaudioplayer_worker.cpp`, CMake files only for
  adding the new source, `docs/bug/asio-status.md`.
- Do not touch: WASAPI files, source preparation, UI files, smoke scripts.
- Validation commands:

```powershell
.\scripts\build-app-msvc.cmd -BuildDir build-structure-asio-worker -Configuration Debug
scripts/run-playback-smoke.ps1 -BuildDir build-structure-asio-worker -Configuration Debug -AsioOutputIndex <known-local-index>
scripts/test-harness-reports.ps1 -LatestSmoke
```

- Expected evidence/status update: update `docs/bug/asio-status.md`; do not
  claim audible endpoint quality without endpoint evidence.

### Slice F: WASAPI worker declaration/implementation split

- Objective: move `WasapiOutputWorker` method bodies from the header to a `.cpp`
  while keeping lifecycle-sensitive methods together.
- Allowed files: `src/backends/wasapi/windowswasapiaudioplayer_worker.h`,
  `src/backends/wasapi/windowswasapiaudioplayer_worker.cpp`,
  `src/backends/wasapi/windowswasapiaudioplayer_worker_helpers.h`, CMake files
  only for adding the new source, `docs/bug/wasapi-anomaly-status.md`.
- Do not touch: ASIO files, UI files, smoke scripts, `WindowsWasapiAudioPlayer`
  high-level transaction files unless the task explicitly expands scope.
- Validation commands:

```powershell
.\scripts\build-app-msvc.cmd -BuildDir build-structure-wasapi-worker -Configuration Debug
scripts/run-playback-regression.ps1 -BuildDir build-structure-wasapi-worker -Configuration Debug -CaseFilter wav-play-stop -NoCleanup
scripts/test-harness-reports.ps1 -LatestRegression
```

- Expected evidence/status update: update
  `docs/bug/wasapi-anomaly-status.md`. State the evidence layer and keep any
  pop/click conclusion limited unless manual or loopback endpoint evidence is
  present.

## Guardrails for all split tasks

- Prefer moving code unchanged before refactoring code.
- Keep one split boundary per task and one backend per branch.
- Add CMake source entries only when a task actually creates C++ source files.
- Do not rename existing classes or public file names unless a later task gives
  an explicit reason.
- Do not mix mechanical splits with behavior fixes.
- Use checked-in scripts for validation and report exact commands, results,
  logs, and report paths.
- If a task only updates this plan or other durable docs, do not update
  `docs/bug/*` trackers unless it changes current investigation state.
