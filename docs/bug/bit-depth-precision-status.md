# Bit-depth precision output status

This file tracks bit-depth precision output implementation, noise shaping,
and the roadmap for full bit-depth-aware playback across WASAPI and ASIO
backends.

## Durable guidance

- Bug/status tracking index: `docs/bug/README.md`
- WASAPI anomaly status: `docs/bug/wasapi-anomaly-status.md`
- ASIO status: `docs/bug/asio-status.md`
- Workflow and change scope: `docs/dev/agent-workflow.md`

## Status refresh: 2026-06-02 (merged codex 24-bit clipping fixes)

- **Branch merge**: Merged `codex-0601-wasapi-bitdepth` into `opencode-0528`.
  The merge includes the 24-bit clipping fix from codex commit `716f827`.
- **Changes merged**:
  - Corrected shared ASIO noise-shaper dither helper (signed two-bit TPDF)
  - WASAPI exclusive Int32/24 quantization with boundary checks
  - Validation evidence from codex's G5 and Realtek test runs
- **Conflict resolution**: Resolved two conflicts in
  `windowswasapiaudioplayer_worker.h` for Int32/24 quantization paths.
  Selected `roundedQuantize32To24()` with boundary checks.
- **Status update**: This merge brings the opencode branch current with
  codex's WASAPI bit-depth precision work.
- **Branch**: `opencode-0528` (current working branch).

## Status refresh: 2026-06-01 (WASAPI exclusive TrueHD clipping fix)

- **Reproduction**: `I:\POWDER SNOW Live V9.8.6.mlp` reproduced severe
  submitted-PCM clipping in WASAPI exclusive mode before the fix. The first G5
  render-mirror window reported `artifactCount=35`, `peak=1`, and RMS near
  `0.499`. Report:
  `build-codex-wasapi-bitdepth/cache/logs/player-smoke-20260601-112647-975-5516221e.harness.json`.
- **Negotiated path**: both Sound BlasterX G5 and Realtek USB Audio selected
  packed `Int24/24`, not `Int32/24`, for this 24-bit TrueHD source. The decoder
  still emits `pcm_s32le`, logged as `decoderBits=32 decoderValidBits=32`.
- **Root cause**: the 32-to-24 noise-shaper returned a left-aligned 32-bit value,
  but the packed 3-byte writer consumed it as an unscaled 24-bit value and
  clamped most nontrivial samples. A second shared defect made the dither helper
  subtract unsigned integers, so its `-1` case wrapped to approximately
  `2^32`, forcing noise-shaped output toward full scale.
- **Fix**: keep quantizer output left-aligned in its 32-bit container, divide by
  256 only when writing packed `Int24`, and generate signed two-bit TPDF dither.
  `Int32/24` handling was also completed: compare valid-bit layout explicitly,
  decode into `Int32/32`, and quantize into the left-aligned 24 valid bits
  required by `WAVEFORMATEXTENSIBLE`.
- **Official format rule**: Microsoft documents that when
  `wValidBitsPerSample` is smaller than `wBitsPerSample`, valid bits are
  left-aligned and unused bits must be zero. Reference:
  https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ksmedia/ns-ksmedia-waveformatextensible
- **Validation PASS**: G5 default-output WASAPI exclusive TrueHD smoke:
  `scripts\run-playback-smoke.ps1 -Source 'I:\POWDER SNOW Live V9.8.6.mlp'
  -BuildDir build-codex-wasapi-bitdepth -Configuration Debug -ExclusiveMode
  -QuitAfterMs 9000 -RequirePlaying -RejectPlaybackErrors
  -RequireExactBitDepthMatch -NoCleanup`. Report:
  `build-codex-wasapi-bitdepth/cache/logs/player-smoke-20260601-112938-781-81927f2b.harness.json`.
  Submitted PCM was clean with no render-mirror artifacts or internal glitch
  candidates.
- **Validation PASS**: G5 WASAPI exclusive 32-bit fixture fallback to packed
  `Int24/24`: report
  `build-codex-wasapi-bitdepth/cache/logs/player-smoke-20260601-112954-804-83b6a89d.harness.json`.
  The log records `sourceBitDepth=32 bitDepthMatch=fallback`,
  `decoderValidBits=32`, and `submitted PCM clean`.
- **Validation PASS**: shared-mode 16-bit fixture regression:
  `build-codex-wasapi-bitdepth/cache/logs/player-smoke-20260601-113018-904-29da4c05.harness.json`.
- **Realtek submitted-PCM check**: explicit Realtek USB Audio WASAPI exclusive
  TrueHD selection also produced clean submitted PCM:
  `build-codex-wasapi-bitdepth/cache/logs/player-smoke-20260601-112851-867-9e4a6ee4.harness.json`.
  Its top-level harness result remains `FAIL` because selecting a device before
  a source creates an unresolved `inactive-source` transition report entry;
  this is separate bookkeeping behavior.
- **Evidence limit**: render mirrors prove the submitted backend PCM is clean.
  They do not prove actual headphone or speaker output is pop-free. Endpoint
  acoustic verification remains `INCONCLUSIVE` until manual listening or
  loopback capture is performed.
- **Build follow-up**: the checked-in FFmpeg source is `8.1.1`, matching the
  latest release checked on 2026-06-01 at https://ffmpeg.org/releases/. The
  original reused slim runtime was stale `7.1.1`; build stamps did not include
  source identity. The build script now stamps source `RELEASE` and `configure`
  SHA-256 and prints `runtimeVersion` on reuse. The isolated runtime and
  deployed bundle both report `8.1.1`.
- **8.1.1 validation PASS**: G5 WASAPI exclusive 16-bit tone:
  `build-codex-validation-ffmpeg811/cache/logs/player-smoke-20260601-122341-667-e39c63a5.harness.json`.
  G5 WASAPI exclusive TrueHD:
  `build-codex-validation-ffmpeg811/cache/logs/player-smoke-20260601-122353-095-c83e67a2.harness.json`.
  The TrueHD path selected exact packed `Int24/24`, submitted clean PCM, and
  completed a zero-final-sample stop fade.

## Phase 1: source-bit-depth-first matching + noise shaping (2026-05-30)

### Completed changes

**Interface**: `setSource()` now accepts `int sourceBitDepth`. The value comes
from `AudioInfo::bitDepthValue` (already probed by
`playbacksourceservice_probe.cpp`). All backends (WASAPI, ASIO, ffmpeg,
native stub) updated.

**WASAPI exclusive mode**: `exclusivePcmCandidates()` now inserts
source-matching encodings before the existing fallback chain:

- 16-bit source → Int16/16 first
- 24-bit source → Int24/24 first, Int32/24 second
- 32-bit source → Int32/32 first, Float32/32 second
- 8-bit source → UInt8/8 first

First `IsFormatSupported()` match wins. If no source-matching format is
supported, falls through to existing candidates (mix format, Int24, Int32/24,
Int32/32, Int16, Float32).

**WASAPI shared mode**: `candidateSampleFormats()` now prefers
source-matching Qt sample formats:

- 16-bit source → Int16 first
- 24-bit source → Int32 first (Qt has no Int24)
- 32-bit source → Int32 + Float first

**2nd-order LNS noise shaping**: Added for 32→24 bit conversion in both
backends:

- WASAPI: `noiseShapedQuantize32To24()` in `copyConvertedFramesToRenderBuffer()`
- ASIO: `noiseShapedQuantize32()` in `writeChannel()`

Uses Lipshitz 2nd-order coefficients (2.0, -1.0) with TPDF dither (±1 LSB).
Per-channel state persists across render callbacks. Reset on format change,
session switch, or driver reopen.

**ASIO bit-depth preference**: `selectOutputFormat()` prefers Int16 when
source bit depth ≤ 16. Logged as `asio source-bit-depth-preference`.

**Logging**: Exclusive candidate and final format logs include
`sourceBitDepth` and `bitDepthMatch=exact|fallback`.

### Current data flow

```
probe → bitDepthValue → setSource(sourceBitDepth)
                              ↓
            ┌─────────────────┴─────────────────┐
       WASAPI 独占                           ASIO
  exclusivePcmCandidates()          selectOutputFormat()
  源位深编码排第一位                16-bit 源 → Int16
            ↓                              ↓
  IsFormatSupported() 逐个探测    getChannelInfo() 原生类型
            ↓                              ↓
  精确匹配 → memcpy 直通          精确匹配 → 直通
  不匹配 → 降级候选               不匹配 → writeChannel()
            ↓                              ↓
  32→24 截断时                    32→24 / 32→16 时
  noiseShapedQuantize32To24()     noiseShapedQuantize32()
  2阶 LNS + TPDF dither          2阶 LNS + TPDF dither
```

### Known limitations

- **Shared mode has no Int24** (Qt limitation). 24-bit source in shared mode
  uses Int32 (32-bit container), not exact match.
- **ASIO has no bit-depth probe API**. Driver native type only known after
  `createBuffers()` + `getChannelInfo()`. Cannot pre-negotiate.
- **Noise shaping only applies to fallback conversions** (32→24). Exact
  matches (e.g. 16-bit source → 16-bit device) are memcpy passthrough.
- **32→16 conversion uses swresample TPDF dither**, not custom noise shaping.
  See Phase 2.
- **Volume ramp and gain adjustment** still operate on the signal, so
  bit-perfect output is not guaranteed even with exact bit-depth match.

### Files changed

| File | Change |
|------|--------|
| `src/core/audioplayerbackend.h` | `setSource` +`sourceBitDepth` param |
| `src/core/nativeaudioplayerstubbase.h` | declaration + `m_sourceBitDepth` |
| `src/core/nativeaudioplayerstubbase.cpp` | implementation |
| `src/backends/wasapi/windowswasiaudioplayer.h` | declaration + `m_sourceBitDepth` |
| `src/backends/wasapi/windowswasapiaudioplayer.cpp` | `setSource` impl + logs |
| `src/backends/wasapi/windowswasapiaudioplayer_output.cpp` | `exclusivePcmCandidates` source-depth-first; `candidateSampleFormats` depth-aware; logs |
| `src/backends/wasapi/windowswasapiaudioplayer_worker.h` | `NoiseShaperState`; `noiseShapedQuantize32To24()`; s32→s24 noise shaping |
| `src/backends/asio/windowsasioaudioplayer.h` | declaration + `m_sourceBitDepth` |
| `src/backends/asio/windowsasioaudioplayer.cpp` | `setSource`; `selectOutputFormat` depth pref; `configureOutput` +`sourceBitDepth`; `noiseShapedQuantize32()`; `writeChannel` noise shaping |
| `src/backends/ffmpeg/ffmpegaudioplayer.h` | declaration + `m_sourceBitDepth` |
| `src/backends/ffmpeg/ffmpegaudioplayer.cpp` | `setSource` impl |
| `src/ui/mainwindow_media.cpp` | `setSource` call + `bitDepthValue` |
| `src/ui/mainwindow_output.cpp` | `setSource` call + `bitDepthValue` |

### Build verification

- Debug build passed: `build-bitdepth` directory
- Build command: `scripts\build-app-msvc.cmd -BuildDir build-bitdepth
  -Configuration Debug -FfmpegAudioCoreRoot
  build-mm/ffmpeg-audio-core/runtime-with-ffprobe-msvc`

---

## Phase 2a: WASAPI 32→16 noise shaping (2026-05-31)

**Completed changes**:

1. `decoderFormatForOutput()` (`windowswasapiaudioplayer_state.cpp`):
   - Int16 now upgrades to Int32, same pattern as Int24→Int32
   - Decoder outputs s32 for 16-bit devices

2. ffmpeg CLI path: automatically uses `-acodec pcm_s32le -f s32le` via
   `m_decoderPcmFormat` (no code change needed).

3. libav in-process path: `sampleFormatForOutput()` receives Int32 (upgraded),
   returns `AV_SAMPLE_FMT_S32` automatically (no code change needed).

4. Render layer (`windowswasapiaudioplayer_worker.h`):
   - `canRenderBufferFormatToDeviceFormat()`: added s32→s16 path
   - Active switch bridge guard: added s32→s16 support
   - `noiseShapedQuantize32To16()`: new function (lsb=65536, shift=16)
   - `copyConvertedFramesToRenderBuffer()`: added s32→s16 conversion with
     noise shaping and rounding-bias fallback

**Files changed**: `windowswasapiaudioplayer_state.cpp`,
`windowswasapiaudioplayer_output.cpp`, `windowswasapiaudioplayer_worker.h`

## Phase 2b: ASIO 32→16 noise shaping (2026-05-31)

**Completed changes**:

1. `AsioOutputWorker`: added `m_decoderFormat` (QAudioFormat) member, separate
   from `m_outputFormat`. Set in `configureOutput()`, cleared when
   `m_outputFormat` is cleared.

2. `configureOutput()`: accepts optional `decoderFormat` parameter. Log
   includes decoder format sample format.

3. `normalizedSample()`: uses `m_decoderFormat` (falling back to
   `m_outputFormat`) to interpret buffer bytes. When decoder outputs s32,
   reads 4-byte samples correctly.

4. Pipeline (`continueStartPipeline()`):
   - Builds `decoderPcmFormat` that upgrades Int16→Int32 when source ≤ 16-bit
   - Builds `decoderQFormat` from the upgraded PcmStreamFormat
   - Passes `decoderQFormat` to `configureOutput()` as decoder format
   - ffmpeg CLI args use `decoderQFormat` for `-acodec`, `-f`, and `aformat`
   - libav in-process receives `decoderPcmFormat` (s32) via `startDecoding()`

**Files changed**: `windowsasioaudioplayer.cpp`

## Noise shaping coverage summary

| Conversion | Backend | Method | Status |
|-----------|---------|--------|--------|
| 32→24 | WASAPI | `noiseShapedQuantize32To24()` in render layer | ✅ Phase 1 |
| 32→24 | ASIO | `noiseShapedQuantize32(shift=8)` in writeChannel | ✅ Phase 1 |
| 32→16 | WASAPI | `noiseShapedQuantize32To16()` in render layer | ✅ Phase 2a |
| 32→16 | ASIO | `noiseShapedQuantize32(shift=16)` in writeChannel | ✅ Phase 2b |
| 24→16 | WASAPI | swresample TPDF dither (decoder outputs s32→render s32→s16) | ✅ via Phase 2a |
| 24→16 | ASIO | swresample TPDF dither + writeChannel noise shaping | ✅ via Phase 2b |

## Verification (2026-05-31)

### Smoke tests

| Source | Backend | bitDepthMatch | Result | Notes |
|--------|---------|--------------|--------|-------|
| 16-bit WAV | WASAPI shared | fallback | PASS | Device is 32-bit; decoder upgrades s16→s32 |
| 24-bit WAV | WASAPI shared | fallback | PASS | Device is 32-bit; decoder upgrades s24→s32 |
| 32-bit WAV | WASAPI shared | exact | PASS | Device is 32-bit; direct match |

All three sources play without errors, audio levels normal (peak=0.016).

### Log verification

- `sourceBitDepth` correctly propagated from probe to output format selection
- `bitDepthMatch` logged correctly (exact when 32→32, fallback when 16/24→32)
- `decoderBits=32` confirmed for all sources (Int24→Int32 and Int16→Int32 upgrades)
- `decoderFormat` parameter passed to ASIO `configureOutput` (logged)

### Regression matrix

- 13/14 gate cases PASS
- 1 FAIL: `wav-output-refresh` (device-refresh edge case, unrelated to bit-depth)
- 7 SKIPPED: local media (eb3/mlp) not available in test build
- No playback errors, no buffer underruns, no stale session writes

### WASAPI exclusive verification

**Realtek USB Audio:**

| Source | Exclusive format | bitDepthMatch | Result |
|--------|-----------------|--------------|--------|
| 16-bit WAV | Int16/16 | exact | PASS |
| 24-bit WAV | Int24/24 | exact | PASS |
| 32-bit WAV | Float32/32 | exact | PASS |

Device supports 16/24/32-bit in exclusive mode; all exact matches.

**Sound BlasterX G5:**

| Source | Exclusive format | bitDepthMatch | Result |
|--------|-----------------|--------------|--------|
| 16-bit WAV | Int16/16 | exact | PASS |
| 24-bit WAV | Int24/24 | exact | PASS |
| 32-bit WAV | Int24/24 | **fallback** | PASS |

G5 does not support 32-bit in exclusive mode. 32-bit source falls back to
Int24/24 via source-bit-depth-first candidate ordering. This exercises the
**32→24 noise shaping path**: decoder outputs s32, device accepts Int24/24,
WASAPI render layer applies `noiseShapedQuantize32To24()`.

Key log evidence:
```
exclusive-candidate ... bits=24 validBits=24 encoding=3 sourceBitDepth=32 bitDepthMatch=fallback
exclusiveMode initialized session=1 rate=48000 channels=2 bits=24 validBits=24
```

### ASIO verification (Creative Sound Blaster G5)

| Source | outputFormat | decoderFormat | Noise shaping | Result |
|--------|-------------|--------------|--------------|--------|
| 16-bit WAV | Int16 (2) | Int32 (3) | ✅ active | PASS |
| 24-bit WAV | Int32 (3) | Int32 (3) | ✅ active | PASS |
| 32-bit WAV | Int32 (3) | Int32 (3) | ❌ not needed | PASS |

Key confirmations:
- `asio noiseShaping enabled` logged for 16-bit and 24-bit sources
- `asio source-bit-depth-request sourceBitDepth=16` triggers Int16 output format
- `configureOutput decoderFormat=3` confirms format separation (decoder s32, output s16)
- All sources: `submitted PCM clean`, no artifacts
- Driver native type: `ASIOSTInt32LSB` (type=18), 8 channels

### Limitations

- WASAPI shared mode on Realtek USB Audio is 32-bit; no bit-depth reduction
  occurs. Verified via exclusive mode and ASIO instead.
- `RequireExactBitDepthMatch` and `RequireNoiseShaping` harness assertions are
  implemented but not yet used in regression cases.
- Exclusive mode `app-report-fail` at playback end is a known buffer behavior
  issue, not related to bit-depth.

### Harness changes

- `ensure-playback-fixtures.ps1`: added 24-bit and 32-bit WAV fixture generation
- `playback-smoke-evidence.ps1`: extracts `bitDepthMatch`, `sourceBitDepth`,
  `outputBits`, `noiseShapingEnabled` from logs
- `playback-smoke-assertions.ps1`: added `-RequireExactBitDepthMatch` and
  `-RequireNoiseShaping` parameters
- `run-playback-smoke.ps1`: passes new parameters through to assertions
