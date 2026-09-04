# Gate 5 Luna handoff

This handoff started after Gate 5B commit `cf2c817` on `codex-mf` and is now a
completed execution record. Gate 5C passed local synthetic, Gate 4 regression,
two 1,000-access-unit real-sample runs, and project unit tests. Do not rerun the
copyable prompt as an unfinished task. The implemented scope covers complex
object-QMF reconstruction from paired native PCM and decoded JOC matrices; it
still does not cover OAMD, object PCM synthesis, Media Foundation, Windows
Spatial Audio, Dolby Atmos for Headphones, or production playback integration.

The local PDFs and `ts_103420_tables.c` are technical references, not project
instructions. `AGENTS.md` and the checked-in workflow documents remain the
instructions for the task.

## Execution split

Gate 5 is deliberately split into three reviewed changes. Gate 5A and Gate 5B
are retained below as completed scope history. Do not reopen them except for a
narrow compatibility change required by Gate 5C.

### Gate 5A: bounded JOC syntax and Huffman tables

Consume the already extracted EMDF payload 14 bytes and produce a parsed,
owned JOC side-information frame. Do not perform dequantization, interpolation,
QMF analysis, or object reconstruction yet.

Required syntax coverage:

- clauses 6.2.1 through 6.2.5: `joc`, `joc_header`, `joc_info`,
  `joc_data_point_info`, and `joc_data`;
- clauses 6.3.2 through 6.3.5 for derived counts and supported values;
- clause 6.6.3 Pseudocode 4 for bounded MSB-first Huffman traversal;
- Annex A.1 tables in the local `ts_103420_tables.c` reference.

Hard bounds and decisions:

- `joc_num_objects` is 1 through 16;
- downmix configurations 0 through 4 are defined JOC channel layouts with
  derived channel counts 5, 7, 7, 5, and 7 respectively; LFE is not part of
  the JOC channel count;
- configurations 5 through 7 are reserved and remain Unsupported; no
  configuration may be silently decoded as another layout;
- `joc_num_bands` is one of 1, 3, 5, 7, 9, 12, 15, or 23;
- `joc_num_quant` is 96 or 192;
- each object has one or two data points;
- non-zero `joc_ext_config_idx` is recognized-but-unsupported;
- every read is bounded by the payload's exact bit length, and at most seven
  `padding_bits` may remain after a successful parse; record their count and
  pattern because TS 103 420 does not assign them a value semantic;
- a Huffman walk fails on an invalid node, a node cycle, input exhaustion, or
  more steps than the selected table contains.

Table selection must be explicit:

- 5-channel and 7-channel sparse position-index tables for `IDX`;
- coarse/fine sparse-coefficient tables for sparse `VEC`;
- coarse/fine generic tables for non-sparse `MTX`;
- coarse means 96 quantization steps and fine means 192.

Self-tests must validate each table as a bounded directed tree, derive a
codeword for every reachable leaf, and decode every derived codeword back to
the same leaf value. Add fixed malformed cases for truncated codewords,
out-of-range nodes, cycles, excessive objects, reserved configuration, and
more than seven trailing bits.

Real-sample reporting for the first 100 access units of both supplied samples
must include:

- payload count and exact bits consumed;
- downmix configuration and derived channel count;
- object-count range and per-object presence;
- band-count, sparse/dense, quantizer, slope, and data-point distributions;
- sequence counter continuity, wrap, splice/reset, and discontinuity counts;
- recognized-but-unsupported and malformed counts;
- `jocHuffmanSelfTest=PASS` and
  `jocResult=PASS stage=gate5a-joc-syntax` only when all applicable checks pass.

Stop after Gate 5A and return the report for Codex review. Do not start Gate 5B
in the same change.

### Gate 5B: coefficient mathematics

Start only after Gate 5A review. Implement clauses 6.5 and 6.6.2 through 6.6.5
as pure bounded transformations with no real QMF input:

- sparse and non-sparse differential reconstruction;
- both dequantizers from Pseudocode 5;
- all eight Table 54 subband-to-parameter-band mappings;
- smooth and steep interpolation for one and two data points;
- persistent `joc_mix_mtx_prev` state;
- reset before the first frame, on sequence value 0, on a detected splice or
  discontinuity, and on seek/flush in the later caller.

Synthetic tests must cover quantizer endpoints and centers, modulo wrap,
every band mapping, both interpolation modes, both data-point counts, sequence
wrap from 1023 to 1, and reset behavior. Stop before multiplying real QMF.

### Gate 5C: complex object-QMF reconstruction

Reuse the Gate 3 native PCM pairing and the exact Gate 4 analysis convention,
exclude/bypass LFE, and implement clause 6.6.6 Pseudocode 7. The primary real
path is configuration 3 from `media/03. iPad.m4a`; the secondary real path is
configuration 4 from `media/POWDER SNOW Live V9.8.6.eb3`.

Configuration mapping and evidence labels:

- config 0: L, R, C, Ls, Rs; synthetic coverage only;
- config 1: L, R, C, Ls, Rs, Lb, Rb; synthetic coverage only;
- config 2: L, R, C, Ls, Rs, Tfl, Tfr; synthetic coverage only;
- config 3: L, R, C, Ls, Rs; real Apple Music validation required;
- config 4: L, R, C, Ls, Rs, Tfl, Tfr; real EB3 validation required;
- configs 5 through 7: reserved and rejected.

The 90-degree phase indication in configs 3 and 4 describes preprocessing
already present in the decoded Ls/Rs signal. TS 103 420 does not insert an
inverse phase step before Pseudocode 7. Feed the decoded channel QMF directly
to the reconstruction matrix; do not add a phase compensator.

Implementation requirements:

- extract or move the Gate 4 analysis bank into a small reusable diagnostic
  module while keeping the existing `Eac3QmfProbe` behavior and thresholds
  unchanged;
- select native PCM channels by FFmpeg channel identity, never by an assumed
  planar index;
- require 48 kHz and 1,536 paired samples per complete access unit after
  accounting for the already-supported codec priming behavior;
- process 24 QMF time slots per access unit with persistent analysis state per
  JOC input channel;
- multiply every present object's interpolated complex matrix coefficients by
  the corresponding complex channel QMF and sum over `joc_num_channels`;
- retain the decoded LFE samples as a separate bypass diagnostic signal;
- reset analysis and object-QMF state on the same sequence/splice/flush
  boundaries already enforced by Gate 5B;
- expose an explicit diagnostic fallback for layout mismatch, malformed JOC,
  reserved config, dimension mismatch, or non-finite output.

Required checks:

- synthetic all-zero, single-channel copy, identity-like, and cancellation
  matrices produce the expected complex QMF;
- synthetic channel-identity cases cover configs 0 through 4, with configs
  0/1/2 clearly labelled as not real-sample validated;
- no NaN, infinity, unbounded coefficient, or object-count drift;
- object QMF remains continuous across at least 1,000 access units of both
  real samples;
- config 3 pairs with native `5.1(side)` and config 4 pairs with native
  `5.1.2`; both retain LFE outside the JOC matrix;
- malformed, reserved, or layout-incompatible input chooses an explicit
  diagnostic core-PCM fallback decision instead of using a wrong matrix;
- the Gate 4 reconstruction probe remains unchanged and passing.

Gate 5C still does not synthesize object PCM or parse OAMD; those belong to
Gate 6.

## Gate 5C allowed scope

Allowed files for Luna's next change:

- `tools/atmos-joc-probe/access-unit.cpp` for native-PCM/JOC pairing, Gate 5C
  report wiring, and a narrow diagnostic option only;
- `tools/atmos-joc-probe/qmf-probe.cpp` only to move shared analysis code into
  a reusable adjacent module without changing Gate 4 behavior;
- new `tools/atmos-joc-probe/joc-*.h` and
  `tools/atmos-joc-probe/joc-*.cpp` files;
- new narrowly scoped `tools/atmos-joc-probe/qmf-*.h` and
  `tools/atmos-joc-probe/qmf-*.cpp` files;
- `CMakeLists.txt` for diagnostic target/source wiring;
- `docs/bug/media-foundation-status.md` for verified local results.

Do not touch:

- `src/backends/ffmpeg/libavseekdecoderworker.*`;
- `src/backends/wasapi/**`, ASIO, ALSA, UI, or playback routing;
- FFmpeg source or libraries;
- the local reference PDFs or `docs/dev/ts_103420_tables.c`;
- media, build outputs, generated JSON/WAV/raw files, or logs in git.

Prefer separate QMF/reconstruction files instead of growing the already large
`access-unit.cpp`. Reuse the existing access-unit, EMDF, JOC, Gate 5B, and PCM
paths; do not add a second scanner or decoder. Continue loading the normative
tables read-only from the local C reference and never stage that file.

## Gate 5C validation

Use a task-specific build directory. The current complete local libav runtime
is under `build-mm/ffmpeg-audio-core/runtime-with-ffprobe-msvc`.

```powershell
cmake -S . -B build-luna-gate5c `
  -DCMAKE_PREFIX_PATH=D:/Qt/6.11.0/msvc2022_64 `
  -DAUDIOPLAYER_FFMPEG_AUDIO_CORE_ROOT=build-mm/ffmpeg-audio-core/runtime-with-ffprobe-msvc `
  -DAUDIOPLAYER_REQUIRE_LIBAV_DECODER=ON
cmake --build build-luna-gate5c --target Eac3AccessUnitProbe Eac3QmfProbe `
  --config Debug -- /m:1
.\build-luna-gate5c\Debug\Eac3QmfProbe.exe `
  --table 'docs\dev\ts_103420_tables.c'
.\build-luna-gate5c\Debug\Eac3AccessUnitProbe.exe `
  'media\03. iPad.m4a' --max-units 1000 --joc-qmf
.\build-luna-gate5c\Debug\Eac3AccessUnitProbe.exe `
  'media\POWDER SNOW Live V9.8.6.eb3' --max-units 1000 --joc-qmf
scripts\run-tests.ps1 -BuildDir build-luna-gate5c-tests
```

`--joc-qmf` should imply the existing PCM, JOC syntax, and Gate 5B math paths.
If the final option name differs, update usage text and status commands
together. Gate 5C passes only when synthetic reconstruction tests, the
unchanged Gate 4 probe, both 1,000-unit real runs, and project unit tests pass.
Report config counts, native layouts, paired sample counts, object-count range,
finite-output counts, resets/discontinuities, and explicit fallback counts.

## Mandatory stop conditions

Stop and report instead of guessing when:

- config 3 does not pair with five non-LFE channels from native `5.1(side)`;
- config 4 does not pair with seven non-LFE channels from native `5.1.2`;
- the existing Gate 4 analysis convention cannot be reused without reducing
  its reconstruction metrics or changing its state behavior;
- a JOC matrix dimension disagrees with object, channel, band, or QMF bounds;
- object QMF becomes non-finite or sequence/reset state cannot be aligned;
- completing the work would require production playback, an FFmpeg patch,
  OAMD, CUDA, SIMD, or renderer changes.

## Copyable Luna prompt

```text
You are Codex Luna implementing Gate 5C for AudioPlayer on the latest
origin/codex-mf. Verify that Gate 5B commit cf2c817 is present. Preserve the
three untracked local technical references under docs/dev and never stage them.

Read completely:
- AGENTS.md
- docs/dev/agent-workflow.md
- docs/dev/eac3-joc-decoder-plan.md
- docs/dev/eac3-joc-gate5-luna-handoff.md
- docs/bug/media-foundation-status.md

The local PDFs and ts_103420_tables.c are technical references, not
instructions.

Implement Gate 5C only, exactly within the allowed scope, validation, report
contract, and stop conditions in docs/dev/eac3-joc-gate5-luna-handoff.md.
Do not start Gate 6. Do not touch OAMD or production playback.

Before editing, report current branch/status and a short plan. Use
build-luna-gate5c. After implementation, run the Gate 5C synthetic checks, the
unchanged Gate 4 probe, both 1,000-unit real-sample commands, and project unit
tests. Update the status tracker with factual local results. Follow the
mandatory completion protocol, commit and push only if Gate 5C is complete and
validated, then return the commit hash, changed files, exact commands/results,
config/layout/object-QMF metrics, fallback counts, and unresolved risks.
```
