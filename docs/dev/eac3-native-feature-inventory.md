# Native E-AC-3 N0 feature inventory

Status: **PASS (N0D bounded feature inventory)**. The probe consumes reserved
bap1/2/4 composite words at their fixed 5/7-bit cursor widths and records
conformance warnings, but it does not assign a mantissa value. This is an N0
inventory exception only; it is not a claim that the bitstream is conforming
or that a future coefficient decoder may clamp or decode these values. The
parser does not decode coefficient values, perform IMDCT/PCM, or apply DRC.
The supplied 1000-AU prefixes activate no SPX/AHT/coupling/enhanced-coupling,
GAQ, or DBA branch; those branches remain structured `Unsupported` when
encountered. N0 inventory PASS is separate from decoder/PCM capability.

## Normative boundary

The implementation was checked against local ETSI TS 102 366 V1.4.1
(2017-09), Annex E and clauses 6.1-6.3:

- `bsi()` is bounded by the normalized syncframe and reports `compr` and
  dual-mono `compr2` presence separately; neither is applied.
- `audfrm()` uses the six-block conditional fields and Table E.1.9 frame
  exponent strategies, including converter, transient, attenuation, and block
  start syntax.
- `audblk()` consumes block switch/dither, `dynrng`/`dynrng2` presence and
  words, SPX/coupling strategy boundaries, rematrix flags, channel bandwidth,
  exponent groups, gain range, allocation reuse, SNR/fast-gain state, skip
  fields, and ordinary mantissa widths.
- Grouped bap 1/2/4 cursor state is shared across exponent sets in one audio
  block and reset only at block end; a partial final group is dummy padding and
  consumes no extra bits. Reserved composite codes are consumed at their
  fixed widths and counted as `reservedGroupWarnings[bap1|bap2|bap4]`, with a
  deterministic first frame/block/channel/LFE/coefficient/bit/code/maxCode
  record. DRC metadata is counted only.
- The N0A audit aligned SNR strategy-2/3 reuse to per-channel prior state,
  retained fast-gain values when a later `fgaincode` is zero, used the current
  coarse SNR value for LFE allocation, and shared grouped cursor ownership with
  LFE. N0C corrected the critical-band-45 width in the native bit-allocation
  table from 12 to the normative 24 bins; the post-fix oracle comparison
  matched all five ordinary config-3 FBW channel BAP sequences.
- Every result carries disposition, stage, reason, and absolute bit position;
  the frame end is the hard reader limit.

## Self-test and sample evidence

`Eac3NativeAudblkProbe --self-test` passes 9 cases: positive synthetic
six-block frame, exponent reuse, grouped 1/2/4 cursor continuation, explicit
LFE grouped-cursor ownership, reserved bap1/bap4 warning plus continued
cursor/later syntax, three tail truncation points, frame-boundary rejection,
and legacy rejection.

The development-only config-3 extraction from `media/03. iPad.m4a` is bounded
by the FFmpeg stream-copy route, not production native demux. The 1000-AU
run on `build-gate8a/n0-extract/ipad-config3-1001.eac3` reports 1000 E-AC-3
frames, 6000 blocks, 1000 accepted, 0 unsupported, and 0 malformed. It has
18 active block-switch events, 29964 dither-on channel/block instances, 36
dither-off instances, 6000 audblk dynrng words, BSI `compr` in 1000 frames,
and no `compr2` or `dynrng2`. `bamode` syntax is present in 653 frames/3918
blocks; exponent strategy/reuse, SNR offsets, fast gain, and DRC metadata
presence are present in all 1000 frames/6000 blocks. All advanced-tool feature
counters are zero and no reserved-group warnings occurred.

The raw config-4 file `media/POWDER SNOW Live V9.8.6.eb3` and every supplied
DEE variant (`1152k`, `1280k`, `1408k`, `1512k`, `1536k`, `1664k`) report the
same bounded result: 2000 syncframes (1000 legacy look-ahead frames and 1000
E-AC-3 frames), 1000 accepted E-AC-3 frames, 0 unsupported, 0 malformed, and
6000 blocks/1000 access units. Each has 24000 dither-on instances, 6000
audblk dynrng words, BSI `compr` in 1000 frames, no `compr2`/`dynrng2`, and
`bamode` syntax in 283 frames/1698 blocks. Exponent strategy/reuse, SNR
offsets, fast gain, and DRC metadata presence are present in all 1000
E-AC-3 frames/6000 blocks. Block switching, transient processing, leak
terms, rematrixing, SPX, AHT, GAQ, coupling, enhanced coupling, coupling
coordinates, delta bit allocation, and spectral-extension branches are all
zero in these prefixes.

For each sample, two identical `--max-units 1000 --summary` runs exited 0 and
produced byte-identical summaries. Exact SHA-256 values and full key/value
reports are in `build-gate8a/n0d-matrix/*-a.txt` and `*-b.txt`:

| sample | summary SHA-256 | N0 result |
| --- | --- | --- |
| config3 | `AFE8D807AD84DE4F36788C543DB127FA6A531626C5539C6CC320FB244A584138` | PASS |
| raw-config4 | `9357F80A1C18E0791DB82512435BFA1DF24C465091694B51C0F68D83821F3F5A` | PASS |
| DEE 1152k | `E3460DB7AD8CD72ED6BE89787C4CB19E9A667C0CDD888DCF9516F54776E40F7F` | PASS |
| DEE 1280k | `80D5E1077F8399CF739D2EA973CC943091D1FB01A6473AA0DF53859A01D43083` | PASS |
| DEE 1408k | `5723861D7BC90B5EFF4A804F25B2E8284093860B2C88B1A9D5E5BD13390C3DDC` | PASS |
| DEE 1512k | `F926E7B4827385E6342761B03B09EC003B482366C2DE25EB86B002521A51C619` | PASS |
| DEE 1536k | `3F89F8DD342E6722C3356FDFD31C8E2DB4767EFCEB71D7950869940544FFB8CB` | PASS |
| DEE 1664k | `D6C1348182F64833040B08129D224EE061970F97F728761DC7458C862107DCF8` | PASS |

This evidence establishes bounded syntax inventory only. It does not establish
native coefficient-value decoding, PCM, IMDCT, JOC, renderer, or production
demux capability. DRC metadata is counted and remains off.

The independent development oracle was the reviewed local FFmpeg source tree
`build-mm/ffmpeg-src` at commit
`96f82f4fbbdc8f7525672bafbf37616ea5fd76ca` (`libavcodec/ac3dec.c`). Its
E-AC-3 block order and per-channel state rules agree with the post-audit
cursor/state changes, while its decoder tables intentionally accept full
composite lookup ranges. The N0 exception consumes those words but preserves
the warning boundary; it does not clamp, skip, mute, or substitute silence.

The earlier config-3 exponent-underflow was traced to the native critical-band
table (band 45 was incorrectly split at 12 rather than 24 bins), corrected,
and pinned by a boundary regression. The independent FFmpeg oracle then
matched all five FBW channel BAP sequences for frame offset 3072, including
the former index-145 divergence; native and oracle block-1 starts both occur
at relative bit 5629. No sample produced Malformed or an advanced-tool
Unsupported in this N0D matrix. Future N1-N3 work must still stop and return
structured Unsupported at the first active advanced branch.

## Reports

Generated reports live under `build-gate8a/n0d-matrix/`. Default output is
summary-only; `--verbose` is required for per-frame Unsupported lines.

The temporary N0C oracle traces remain outside the source tree under
`build-gate8a/n0c-*`; they were not added to the probe's default output.

## Development extraction

Config-3 was obtained with a development-only stream-copy command:

```powershell
& E:\Tool\ffmpeg\bin\ffmpeg.exe -y -hide_banner -loglevel error -i 'media\03. iPad.m4a' -map 0:a:0 -c copy -frames:a 1001 -f eac3 'build-gate8a\n0-extract\ipad-config3-1001.eac3'
```

This is an offline extraction/oracle route and is not a production native
container demux contract.
