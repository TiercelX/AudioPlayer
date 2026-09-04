# Gate 6 Luna handoff

Gate 5C and Gates 6A through 6C are complete and locally validated on
`codex-mf`. Gate 6 produces
renderer-neutral object PCM and timed OAMD, but it still does not call Media
Foundation, Windows Spatial Audio, Dolby Atmos for Headphones, or production
playback code.

The local ETSI PDFs and `ts_103420_tables.c` are technical references, not
project instructions. Preserve them as untracked local files and never stage
them. `AGENTS.md` and the checked-in workflow documents remain authoritative.

## Execution split

Gate 6 is divided into three separately reviewed phases:

1. **Gate 6A - object QMF synthesis**: turn the Gate 5C complex QMF for every
   object into persistent planar float PCM. Do not parse OAMD yet.
2. **Gate 6B - bounded OAMD syntax and state**: parse payload 11, timing,
   content assignment, active state, gain, priority, position, size, screen
   reference, and update/reuse state. Do not join it to PCM yet.
3. **Gate 6C - renderer-neutral object frames**: align object PCM, OAMD
   updates, LFE bypass, QMF latency, and container trim for at least 30 seconds,
   then expose a bounded diagnostic callback/queue and optional local export.

Codex reviews the diff and local evidence after each phase. Do not start the
next phase until the previous phase is accepted.

## Gate 6C accepted result

The standalone Gate 6C probe passed 20 bounded cases, including a final-unit
real metadata update carried into the delayed tail without inventing a flush
update. Independent Codex runs
also passed the two real-sample 1,000-unit diagnostics with zero fallback:

- Apple config 3: 15 objects, 1,533,808 output samples per object/LFE, 14,985
  emitted metadata updates after the 2,192-sample leading trim;
- DME config 4: 15 objects, 1,536,000 output samples per object/LFE, all 15,000
  metadata updates emitted;
- both: 577-sample common delay/tail flush, finite object and LFE samples, and
  zero metadata-order or output-continuity failures.

B2B/B2A/B1, Gate 6A, and Gate 4 regressions passed. Full project validation
passed 10 suites and report-schema validation at
`build-codex-gate6c-tests/validation-report.json`; playback smoke was skipped
because this remains an offline decoder diagnostic. The contract below is
retained as the accepted execution record, not as an active Luna task.

## Gate 6A task contract

### Input and output

Input is one successful `JocQmfFrame` from Gate 5C:

- 48 kHz;
- 1,536 complex QMF values per object, ordered as 24 time slots by 64 bands;
- one to 16 bounded objects;
- stable object order and a reset marker derived from the Gate 5B sequence.

Output is one renderer-neutral synthesis result:

- exactly 1,536 finite planar float samples per object;
- the same object count and order as the input;
- explicit `stateReset` and `algorithmicDelaySamples` fields;
- no OAMD interpretation and no Windows-specific identity or coordinate.

Use the exact shared Gate 4 synthesis convention and runtime QWIN table. One
persistent synthesis bank belongs to each object. Every object bank advances
for every access unit, including an all-zero QMF unit; an absent/inactive
decision belongs to OAMD later and must not freeze synthesis history.

### State and failure rules

- Reset all object synthesis banks before processing the current frame when
  Gate 5C reports `stateReset`, or when object count changes.
- Validate the complete input dimensions and every complex value before
  mutating persistent synthesis state or caller output.
- A rejected frame must not advance any synthesis bank and must leave caller
  output unchanged.
- Reject zero objects, more than 16 objects, non-1,536 vectors, NaN, infinity,
  and impossible QWIN dimensions.
- The shared Gate 4 analysis/synthesis probe must retain its 577-sample delay,
  reconstruction SNR, split-boundary, and reset invariants.
- Record the 577-sample synthesis-chain latency; do not discard samples, shift
  OAMD, or delay LFE in Gate 6A. That alignment belongs to Gate 6C.

### Performance boundary

The reference QMF implementation is scalar. CUDA and vendor SIMD are not part
of Gate 6A. It is allowed to precompute fixed analysis/synthesis modulation
kernels inside the shared QMF bank when all Gate 4 numerical metrics and exact
state invariants remain unchanged. Do not change the matrix equations or
arithmetic precision to improve speed.

### Synthetic acceptance

Add a standalone `Eac3JocSynthesisProbe` and cover:

- all-zero object QMF produces finite zero PCM;
- a single-object analysis/synthesis chain matches the Gate 4 reference;
- multiple objects remain independent;
- split access-unit processing equals continuous processing;
- explicit reset equals a fresh instance;
- object-count change resets before the current frame;
- a late non-finite input is rejected transactionally and the following legal
  frame matches an untouched reference state;
- output is exactly 1,536 samples per object and the reported delay is 577.

Gate 6A is complete only when the synthesis self-test and unchanged Gate 4
probe pass. A short real Gate 5C-to-synthesis smoke may be added, but a real
30-second object/OAMD result is Gate 6C and must not be claimed here.

## Gate 6B execution split

Gate 6B is split again because element framing and raw conditional syntax can
be proven without guessing object-property reuse semantics:

1. **Gate 6B1 - OAMD framing and inventory**: parse the payload header,
   program assignment, variable-length counts, and byte-bounded element
   metadata; retain each element body as owned raw bits/bytes. Run this over
   both real samples.
2. **Gate 6B2A - raw object update syntax**: parse object-element timing and
   every conditional basic/render codeword without applying previous-update
   reuse, differential values, or normalized property equations.
3. **Gate 6B2B - object property state**: implement default/full/reuse/mixed,
   differential position, and decoded renderer-neutral values. Parse
   trim/extended elements only to the degree required by the supplied streams.

### Gate 6B1 completed contract

Follow clauses 5.5.1 through 5.5.4 and 5.6.0/5.6.4 of ETSI TS 103 420:

- parse `oa_md_version_bits` plus its extension;
- parse `object_count_bits` plus extension and expose `object_count = code+1`;
- enforce the syntax maxima of 159 objects (including the seven-bit escape)
  and 46 OA elements; these are framing bounds, not renderer limits;
- parse `program_assignment`, including dynamic-only/LFE or content
  description, bed instances, ISF index, and dynamic-object count;
- parse `b_alternate_object_data_present` and the extended OA element count;
- parse `oa_element_id_idx`, bounded `oa_element_size`, optional alternate ID,
  and `b_discard_unknown_element`;
- copy the remaining element body under its declared byte boundary without
  interpreting object, trim, or extended-object properties yet;
- require all size padding and final payload padding bits to be zero.

Recognize element IDs 1 (object), 2 (trim), and 5 (extended object). Reserved or
unknown elements are reported as unsupported; a discardable unknown may be
skipped within its declared boundary, while a non-discardable unknown makes
the frame unsupported. Neither case may be treated as malformed unless bounds
or padding are invalid.

Add a bounded parser module and synthetic self-test. Integrate a narrow
`--oamd` diagnostic into `Eac3AccessUnitProbe` so payload ID 11 remains linked
to `unitIndex`. For the first 1,000 access units of both supplied samples,
report payload count, disposition, version, object-count range, program type,
LFE declaration, element ID/size counts, padding, and unit-association count.

Synthetic B1 acceptance must cover normal and extended version/object/element
counts, multi-group `variable_bits_max`, truncated payload, declared-size
overrun, illegal/non-zero padding, recognized elements, discardable unknown,
and non-discardable unknown. All bit reads and length arithmetic are bounded.

Gate 6B1 does not parse `md_update_info`, object active state, gain, position,
size, priority, trim semantics, or extended-object properties. It does not
touch Gate 6A synthesis, production playback, FFmpeg, or Windows rendering.
If either real sample cannot be framed without interpreting an unspecified
element boundary, stop and report its exact element inventory.

### Gate 6B2A completed contract

Consume the owned body of the recognized ID-1 object element from B1 and parse
clauses 5.5.5 through 5.5.11 structurally within that element boundary:

- `md_update_info`, including sample-offset coding and one to eight shared
  `block_update_info` entries;
- block offset and ramp-duration coding;
- the reserved-data-present flag;
- one `object_info_block` per declared OAMD object per update block;
- active/inactive flag and raw basic/render status;
- conditional gain, priority, absolute/differential position, distance, zone,
  size, screen-reference, and snap codewords;
- bounded additional-table data and zero element padding.

Output owned raw update records keyed by `objectIndex` and `blockIndex`. Keep
codewords and presence flags exactly as transmitted. Do not apply default,
reuse, mixed-update, differential-position, gain/coordinate conversion, or
inter-frame history in B2A. Do not interpret the ID-2 trim element yet.

For dynamic-only plus LFE streams, report the per-index active/status
distribution for all 16 OAMD objects. Do not assume which index is the LFE in
the audio-object mapping until the real syntax inventory and the standard's
program ordering agree. Gate 6C will map the 15 JOC essences and LFE bypass.

Synthetic B2A acceptance must cover one and eight update blocks, every
basic/render status and conditional field branch, inactive objects,
absolute/differential position, distance infinity/index, scalar and 3-D size,
screen reference, additional-table data, truncation at late fields, excessive
block/object arithmetic, and non-zero internal padding. All reads are bounded
to the 61-byte element body rather than the enclosing payload.

Integrate a narrow real diagnostic over 1,000 payloads of both samples and
report parse disposition, update-block and sample-offset distributions,
active/inactive counts by object index, basic/render status distributions,
position mode and other conditional-codeword counts, additional-data count,
bits consumed/padding, and unit association. Stop if the two samples require
different undocumented structure or if the 16-object parse cannot consume the
declared element boundary exactly.

B2A must not mutate any persistent object state, decode normalized property
values, synthesize PCM, parse trim semantics, or touch production/Windows
paths. B2B owns those operations after Codex review.

The accepted implementation passed 19 synthetic cases. Across the first 1,000
access units of both supplied samples it parsed 1,000/1,000 ID-1 elements with
zero malformed or unsupported results, 16,000 object records, 487,000 consumed
bits, and 3,000 zero padding bits. Every frame contained one full update block.
Object index 0 was the LFE-only bed helper and indices 1 through 15 were the
active dynamic objects. Scanning every possible single-helper index proved
that index 0 was the only map that consumed the element boundary for both
samples and for both candidate presence-bit interpretations.

Clause 5.5.11 and Table 31 label the four render-presence bits in opposite
directions. B2A therefore records an explicit bit-order policy and defaults to
the clause-5.5.11 LSB order; the Table-31 interpretation remains diagnostic.
The supplied streams use only one-block full updates and cannot distinguish
the two interpretations. B2B must not silently remove this uncertainty.

### Gate 6B2B completed contract

Consume one successful B2A frame and reconstruct renderer-neutral object
property state. This is a metadata state-machine phase only: do not associate
properties with object PCM or implement Gate 6C timing/alignment.

Implement persistent state for every declared object and apply, in standard
order:

- basic-info default, full, reuse, and mixed status, including active state,
  gain, and priority;
- render-info default, full, reuse, and mixed status;
- absolute position followed by bounded differential position relative to the
  correct prior value;
- distance/infinity, zone, scalar or three-dimensional size, elevation,
  screen-reference, and snap properties;
- the standard codeword-to-renderer-neutral conversions and documented
  defaults, while retaining the original raw codewords for diagnostics.

Use the accepted `Syntax5511Lsb` presence-bit policy for normal decoding and
keep `Table31Msb` diagnostic-only. Treat object index 0 in the supplied
dynamic-only-plus-LFE program as the LFE-only bed helper: retain its metadata
identity but do not expose it as one of the 15 dynamic JOC objects. Do not
change the Gate 5C essence order.

State application must be transactional per OAMD frame. Validate every update
block, referenced prior value, converted finite value, and object/program
shape before committing any state or caller output. A late failure must leave
all persistent state and output unchanged. Reset before the current frame on
seek/discontinuity, explicit generation reset, or incompatible object/program
shape change. A reuse or differential operation without valid history must
follow an explicit standard default if one exists; otherwise stop as
unsupported rather than inventing a value.

The one-byte ID-2 trim element may be retained as bounded raw metadata. Parse
trim or ID-5 extended-object semantics only if B2B state reconstruction for a
supplied stream demonstrably requires them; otherwise defer them and state the
limit. Do not infer undocumented semantics from the samples.

Synthetic B2B acceptance must cover:

- fresh-frame defaults, full updates, reuse, and mixed per-property updates;
- multi-frame absolute then differential position and all coordinate bounds;
- gain, priority, distance/infinity, zone, scalar/3-D size, elevation,
  screen-reference, snap, inactive, and reactivation transitions;
- the index-0 LFE helper and stable mapping of the 15 dynamic objects;
- reset and object/program-shape reset equivalence to a fresh instance;
- reuse/differential before valid history, invalid codewords, non-finite
  conversion, and a late malformed update without partial state mutation.

Integrate a narrow diagnostic over 1,000 payloads of both supplied samples.
Report successful state applications, reset/default/reuse/mixed counts,
finite normalized-property counts and ranges, LFE-helper separation, dynamic
object count/order, and unit association. Because both samples currently use
full updates only, reuse, mixed, differential, and reset behavior must remain
explicitly synthetic evidence rather than a real-stream claim.

Stop and report if the standard does not uniquely define a conversion or
history source, the two samples require incompatible state rules, a property
cannot be bounded transactionally, or the work would require PCM alignment,
production playback, a Windows renderer, FFmpeg changes, CUDA, or SIMD.

The accepted B2B implementation passed 37 synthetic cases after Codex review
corrected the coordinate, size, priority, zone, screen/distance, history, and
multi-block contracts. It retains one ordered property snapshot per update
block plus final state. Both supplied samples applied 1,000/1,000 frames with
zero fallback, 15 stable dynamic objects, and one separated index-0 LFE helper.
Their real streams contain only one full/absolute block per frame, so reuse,
mixed, differential, inactive/reactivation, and multi-block behavior remain
synthetic evidence.

## Gate 6C active task contract

Gate 6C joins the already proven diagnostic stages into a renderer-neutral
stream. It must not add a production decoder or call a Windows renderer.

### Timeline and mapping

- Associate exactly one successful payload 11 and payload 14 by access-unit
  index before producing output for that unit.
- Map JOC essence index 0 through 14 to OAMD dynamic object index 1 through 15
  in order. Keep OAMD index 0 and decoded LFE PCM as one separate LFE bypass;
  never synthesize or expose it as a dynamic object.
- Synthesize all 15 object-QMF planes with the persistent Gate 6A banks. Every
  successful access unit contributes exactly 1,536 finite samples per object.
- Convert B2A timing to source sample positions using
  `frame_offset + sample_offset + 32 * block_offset_factor`; retain the ramp
  duration and the corresponding ordered B2B block snapshot.
- The QMF analysis/synthesis chain delays object PCM by 577 samples. Build one
  common aligned timeline by delaying LFE PCM and OAMD event positions by 577.
- Apply container leading `skip_samples` only after object synthesis and common
  alignment. The selected common interval therefore starts at
  `skip_samples + 577` and ends at
  `decoded_source_samples - discard_padding + 577`. This preserves the
  requested source trim while keeping object PCM, LFE, and metadata aligned.
- Flush the synthesis banks with enough zero-QMF input to cover the 577-sample
  tail, but do not emit synthetic metadata or count a flush as a source access
  unit. Validate discard-padding only when the probed range includes the true
  end of the stream.
- Preserve monotonic source/access-unit identity and expose output-relative
  sample positions. If an event falls before the selected start, carry its
  resulting state to the trim boundary rather than emitting a negative event.

### Output and transaction boundary

Add a diagnostic-only bounded assembler and standalone self-test. Its output
contract is a callback or bounded queue of renderer-neutral batches containing:

- planar float PCM for 15 dynamic objects plus one separately identified LFE
  plane, all with equal finite sample counts;
- ordered metadata updates with output-relative sample position, ramp
  duration, dynamic object index, and the normalized B2B property snapshot;
- access-unit/generation identity, source and output sample ranges, trim
  accounting, and an explicit end/flush marker.

Validate the complete unit association, dimensions, finite PCM, object map,
timing arithmetic, and callback capacity before committing state or invoking
the callback. A rejected unit must not advance QMF synthesis, OAMD state,
timeline counters, queued output, or caller-visible metrics. Reset all joined
state before the current unit on seek/discontinuity or generation change.

### Synthetic acceptance

Cover at least:

- identity mapping of 15 JOC essences to OAMD indices 1 through 15 and a
  separate index-0 LFE bypass;
- one and eight metadata blocks, exact `sample_offset + 32 * block_factor`
  positions, ramp duration, ordering, and state carried across leading trim;
- a common 577-sample object/LFE/metadata delay, including impulse alignment;
- leading skip and terminal discard on the common aligned interval, exact
  output length, synthesis tail flush, and no flush metadata;
- split versus continuous batching, monotonic positions/PTS, bounded callback
  capacity, explicit reset, and fresh-instance equivalence;
- late association, non-finite PCM, shape, timing-overflow, and callback
  rejection with no partial state or output mutation.

### Real acceptance

Run both supplied samples for at least 1,000 continuous access units (32 s):

- 1,000 payload-11/payload-14/PCM associations and zero fallback;
- exactly 15 finite dynamic-object planes and one finite LFE bypass;
- 1,000 successful synthesis and B2B applications, plus bounded tail flush;
- 15,000 full/absolute dynamic-object metadata updates for the supplied
  one-block streams, stable mapping, monotonic event positions, and no
  duplicate or missing unit identity;
- exact source, QMF-delay, container-skip, output-length, and optional terminal
  discard accounting. For the Apple sample, the known leading selection point
  is `2192 + 577 = 2769` aligned samples. Do not invent terminal padding for a
  partial 1,000-unit range.

Report finite peaks/ranges and structural alignment only. Internal PCM does not
prove audible output or Dolby Atmos for Headphones rendering. Do not export
large media by default; any optional diagnostic output stays under the ignored
build directory.

Gate 6C must stop if:

- a unit cannot be associated one-to-one across PCM, payload 11, and payload 14;
- the 577/trim/timestamp convention conflicts with measured source length;
- an object/LFE/metadata stream cannot share one exact sample timeline;
- transactional rollback would require mutating the existing Gate 5C/6A/6B
  implementations;
- the work would enter production playback, Media Foundation, Windows Spatial
  Audio, Dolby Atmos for Headphones, FFmpeg source, CUDA, or vendor SIMD.

## Gate 6C allowed scope

Allowed files:

- new adjacent renderer-neutral alignment files under
  `tools/atmos-joc-probe/`;
- a new standalone Gate 6C alignment probe;
- diagnostic integration in `Eac3AccessUnitProbe` and the existing Gate 5C/
  Gate 6A probe-only data flow;
- `CMakeLists.txt` for diagnostic targets;
- Gate 6 plan/status documents after local validation.

Do not modify:

- production playback or decoder workers;
- Media Foundation, WASAPI, ASIO, or ALSA backends;
- FFmpeg source or build scripts;
- proven Gate 4 through Gate 6B algorithms except for narrow immutable-state
  accessors or cloning required for transactional diagnostic composition;
- renderer, CUDA, or SIMD code;
- the three untracked local technical references.

## Gate 6C validation

Use a task-specific build directory and the existing complete libav runtime:

```powershell
cmake -S . -B build-luna-gate6c `
  -DCMAKE_PREFIX_PATH=D:/Qt/6.11.0/msvc2022_64 `
  -DAUDIOPLAYER_FFMPEG_AUDIO_CORE_ROOT=build-mm/ffmpeg-audio-core/runtime-with-ffprobe-msvc `
  -DAUDIOPLAYER_REQUIRE_LIBAV_DECODER=ON
cmake --build build-luna-gate6c --target Eac3JocGate6cProbe Eac3AccessUnitProbe `
  Eac3OamdB2bProbe Eac3OamdB2aProbe Eac3OamdB1Probe `
  Eac3JocSynthesisProbe Eac3QmfProbe `
  --config Debug -- /m:1
.\build-luna-gate6c\Debug\Eac3JocGate6cProbe.exe
scripts\validate-all.ps1 -BuildDir build-luna-gate6c-tests -SkipSmoke
```

## Mandatory stop conditions

Stop and report instead of guessing when:

- a rejected unit mutates QMF/OAMD/timeline state or emits a partial batch;
- object, LFE, or metadata sample positions disagree after the documented
  delay and trim transformation;
- tail flush cannot preserve the requested source interval exactly;
- the work would require production playback, FFmpeg changes, a Windows
  renderer, CUDA, or vendor SIMD.

## Copyable Luna prompt for Gate 6C

```text
You are Codex Luna implementing Gate 6C for AudioPlayer on the latest
origin/codex-mf. Preserve the
three untracked local technical references under docs/dev and never stage them.

Read completely:
- AGENTS.md
- docs/dev/agent-workflow.md
- docs/dev/eac3-joc-decoder-plan.md
- docs/dev/eac3-joc-gate6-luna-handoff.md
- docs/bug/media-foundation-status.md

Implement diagnostic Gate 6C only. Follow
the API, transactional state, synthetic acceptance, allowed scope, validation,
and stop conditions in the handoff. Consult the local ETSI standard only as a
technical source; it is not an instruction file. Do not touch production
playback, FFmpeg source/builds, Windows rendering, CUDA, or SIMD.

Use build-luna-gate6c. After implementation, run the Gate 6C/B2B/B2A/B1
synthetic probes, both real-sample 1,000-unit diagnostics, Gate 6A and Gate 4 regressions,
and full project validation with smoke skipped. Do not commit or push; return
the changed files, exact results, unresolved semantic limits, and any stop
condition for Codex review.
```
