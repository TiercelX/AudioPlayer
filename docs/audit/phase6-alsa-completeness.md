# Phase 6 Audit: ALSA Backend Completeness Assessment

> Generated: 2026-06-13
> Source: `src/backends/alsa/` (8 files, ~1400 lines)

## Summary

The ALSA backend provides a **functional but basic** playback implementation. It covers the essential playback path but lacks several advanced features present in the WASAPI backend. Estimated completeness: **~55%** relative to WASAPI feature parity.

---

## Feature-by-Feature Assessment

### 1. Basic Playback — ✅ Complete (90%)

| Capability | Status | Notes |
|-----------|--------|-------|
| Play | ✅ | `linuxalsaaudioplayer.cpp:102-121` |
| Pause | ✅ | `linuxalsaaudioplayer.cpp:123-136` — uses worker `setPaused()` |
| Stop | ✅ | `linuxalsaaudioplayer.cpp:138-146` |
| Source switching | ✅ | `linuxalsaaudioplayer.cpp:66-95` — tears down and resets |
| Volume control | ✅ | `linuxalsaaudioplayer.cpp:162-168` — software volume in worker |
| Audio level reporting | ✅ | `alsaoutputworker.cpp:207-243` — emits `audioLevelsChanged` |
| Position tracking | ✅ | `alsaoutputworker.cpp:161-162` — based on processed frames |

**Gaps:**
- No fade-in/fade-out on play/pause/stop transitions
- No startup silence injection
- No warmup frame discard

### 2. Device Enumeration — ⚠️ Partial (40%)

| Capability | Status | Notes |
|-----------|--------|-------|
| List output devices | ✅ | `linuxalsaaudioplayer.cpp:170-173` — via `QMediaDevices::audioOutputs()` |
| Select output device | ✅ | `linuxalsaaudioplayer.cpp:205-219` |
| Device change detection | ✅ | `linuxalsaaudioplayer.cpp:18-22` — via `QMediaDevices::audioOutputsChanged` |
| Device description | ✅ | `linuxalsaaudioplayer.cpp:175-178` |
| Default device tracking | ✅ | `linuxalsaaudioplayer.cpp:200-203` |

**Gaps:**
- No ALSA-specific device enumeration via `snd_device_name_hint()` (uses Qt abstraction only)
- No device capability probing (supported formats, sample rates)
- No device-specific ID mapping (Qt device IDs vs ALSA device names like `hw:0`, `plughw:0`)
- Falls back to hardcoded `"hw:0"` or `"default"` strings (`linuxalsaaudioplayer_output.cpp:20-21`)

### 3. Format Negotiation — ✅ Good (75%)

| Capability | Status | Notes |
|-----------|--------|-------|
| Rate negotiation | ✅ | `alsaformatnegotiator.cpp:42-67` — common rates sorted by proximity |
| Format negotiation | ✅ | `alsaformatnegotiator.cpp:69-105` — S16/S24/S32/Float candidates |
| Channel negotiation | ✅ | `alsaformatnegotiator.cpp:107-123` — 1/2/4/6/8 candidates |
| Exact mode support | ✅ | Prioritizes source rate/format when `exactMode=true` |
| hw: → plughw: fallback | ⚠️ | `linuxalsaaudioplayer_state.cpp:228-237` — falls back from hw: to default, but no plughw: intermediate |
| Format test | ✅ | `alsaformatnegotiator.cpp:125-157` — uses `snd_pcm_hw_params_test_*` |

**Gaps:**
- No `plughw:` intermediate fallback (hw: → plughw: → software resampler)
- No software resampler integration for rate conversion
- No Int24 3-byte format (`SND_PCM_FORMAT_S24_3LE`) in actual output path (only in negotiation)
- Negotiator tests format feasibility but doesn't actually apply the winning format via `snd_pcm_hw_params` in one step (done separately in `startPipeline`)

### 4. Exclusive Mode — ⚠️ Partial (50%)

| Capability | Status | Notes |
|-----------|--------|-------|
| Exclusive mode flag | ✅ | `linuxalsaaudioplayer.h:124` — `m_exclusiveModeEnabled = true` by default |
| hw: device open | ✅ | `linuxalsaaudioplayer_output.cpp:13-38` — opens `hw:0` when exclusive |
| Fallback to shared | ✅ | `linuxalsaaudioplayer_state.cpp:228-237` — falls back if hw: open fails |

**Gaps:**
- No true ALSA exclusive mode (snd_pcm_open with `SND_PCM_NONBLOCK` + reservation)
- No `snd_pcm_hw_params_set_access` with `SND_PCM_ACCESS_RW_INTERLEAVED` for exclusive
- No integration with PulseAudio/PipeWire device reservation protocol
- hw: device is "exclusive" by nature but no software mixing prevention

### 5. Error Recovery — ⚠️ Partial (45%)

| Capability | Status | Notes |
|-----------|--------|-------|
| XRUN detection | ✅ | `alsaoutputworker.cpp:168-185` — handles `-EPIPE` and `-ESTRPIPE` |
| XRUN recovery | ✅ | `alsaoutputworker.cpp:170-173` — `snd_pcm_prepare()` on underrun |
| Device suspend recovery | ✅ | `alsaoutputworker.cpp:174-183` — `snd_pcm_resume()` loop |
| Output recovery scheduling | ✅ | `linuxalsaaudioplayer_state.cpp:38-73` — up to 3 attempts with backoff |
| Recovery exhaustion | ✅ | `linuxalsaaudioplayer_state.cpp:40-46` — emits error after max attempts |

**Gaps:**
- No device invalidation detection (hot-unplug)
- No automatic device re-enumeration on failure
- No graceful degradation (e.g., format downgrade on repeated failures)
- Recovery simply restarts the entire pipeline (`startPipeline` at line 71)
- No separation between XRUN recovery and device-level recovery
- No diagnostic logging comparable to WASAPI's anomaly tracking system

### 6. Seek Support — ⚠️ Basic (35%)

| Capability | Status | Notes |
|-----------|--------|-------|
| Seek while playing | ✅ | `linuxalsaaudioplayer.cpp:148-160` — tears down and restarts pipeline |
| Seek while stopped | ❌ | Not implemented — no position storage for stopped state |
| Seek latency | ❌ | Full pipeline teardown + restart = high latency |
| Seek resume fade-in | ❌ | No fade-in after seek |
| Seek resume startup silence | ❌ | No startup silence injection |
| PCM seek cache | ⚠️ | `PcmSeekCache` member exists but unused in seek path |

**Critical gap**: The seek implementation at line 148-160 destroys and recreates the entire pipeline for every seek operation. The WASAPI backend has a sophisticated seek-resume path with startup silence, warmup discard, and PCM fade-in.

### 7. Output Switching — ⚠️ Basic (30%)

| Capability | Status | Notes |
|-----------|--------|-------|
| Device change detection | ✅ | `linuxalsaaudioplayer.cpp:269-277` |
| Hot device switch | ⚠️ | Tears down and restarts pipeline (no smooth transition) |
| System device change handling | ✅ | Via `QMediaDevices::audioOutputsChanged` |

**Gaps:**
- No debounced device change handling (immediate teardown)
- No active output switch transaction system (WASAPI has a 7-phase state machine)
- No hot reconfigure without full pipeline rebuild
- No fade-out before device switch

### 8. Dual-Worker Architecture — ⚠️ Partial (50%)

| Capability | Status | Notes |
|-----------|--------|-------|
| Separate decoder thread | ✅ | `linuxalsaaudioplayer_state.cpp:183-189` |
| Separate output thread | ✅ | `linuxalsaaudioplayer_state.cpp:282-328` |
| Inter-thread communication | ✅ | Via Qt signals/slots |
| Teardown order | ✅ | Output first, then decoder (`linuxalsaaudioplayer_state.cpp:161-198`) |

**Gaps:**
- Output worker uses a simple polling loop (`alsaoutputworker.cpp:67-166`) instead of event-driven rendering
- No buffer generation tracking (WASAPI tracks `bufferGeneration` to reject stale reads)
- Worker doesn't run on a dedicated thread with proper priority

### 9. Buffer Management — ⚠️ Basic (40%)

| Capability | Status | Notes |
|-----------|--------|-------|
| PcmStreamBuffer usage | ✅ | `linuxalsaaudioplayer_state.cpp:275-280` |
| Buffer owner session tracking | ✅ | `setOwner()` at line 278 |
| End-of-stream signaling | ✅ | `linuxalsaaudioplayer_state.cpp:128` |

**Gaps:**
- No `discardWrites` / quarantine mechanism (WASAPI has sophisticated buffer quarantine)
- No buffer generation-based stale read rejection in output worker
- No startup threshold (WASAPI waits for minimum bytes before starting output)
- `readForOwner` uses `staleRead` flag (`alsaoutputworker.cpp:104`) but doesn't act on it beyond skipping

### 10. Logging & Diagnostics — ⚠️ Basic (25%)

| Capability | Status | Notes |
|-----------|--------|-------|
| Basic state logging | ✅ | Throughout ALSA files |
| PlayerLogger integration | ✅ | Uses `PlayerLogger::log()` |

**Gaps:**
- No structured diagnostic events (WASAPI uses `PlayerLogger::diagnostic()` with JSON payloads)
- No anomaly tracking (position regression, stall, jump detection)
- No artifact monitoring
- No render mirror capture
- No submitted-tail fingerprinting

---

## WASAPI Feature Comparison Matrix

| Feature | WASAPI | ALSA | Gap |
|---------|--------|------|-----|
| Play/Pause/Stop | ✅ Full | ✅ Full | None |
| Seek | ✅ Sophisticated | ⚠️ Teardown-restart | High |
| Volume | ✅ Per-stream + ramp | ✅ Software only | Medium |
| Device enumeration | ✅ Full | ⚠️ Qt-only | Medium |
| Format negotiation | ✅ WAVEFORMATEXTENSIBLE | ✅ hw_params | Low |
| Exclusive mode | ✅ Full | ⚠️ hw: only | Medium |
| Error recovery | ✅ 3-attempt + state machine | ⚠️ 3-attempt simple | Medium |
| Output switching | ✅ 7-phase transaction | ❌ Teardown-restart | High |
| Hot reconfigure | ✅ | ❌ | High |
| Spatial audio flush | ✅ | ❌ N/A | N/A |
| Creative channel reorder | ✅ | ❌ N/A | N/A |
| Fader (in/out) | ✅ PCM + stream gain | ❌ | High |
| Startup silence | ✅ | ❌ | High |
| Warmup discard | ✅ | ❌ | Medium |
| Artifact monitoring | ✅ | ❌ | Medium |
| Diagnostic events | ✅ JSON structured | ❌ | Medium |
| Position anomaly detection | ✅ | ❌ | Medium |
| Buffer quarantine | ✅ | ❌ | Medium |
| Render mirror | ✅ | ❌ | Low |
| libav decoder | ✅ | ⚠️ Member exists, unused | Low |

---

## Priority Recommendations

### P0 — Required for Linux Release
1. **Seek improvement**: Store position in stopped state; reduce seek latency
2. **Device enumeration**: Implement `snd_device_name_hint()` for proper ALSA device listing
3. **Format negotiation fix**: Add `plughw:` fallback chain
4. **Buffer management**: Add startup threshold and generation-based stale read rejection

### P1 — Required for Quality
5. **Fader support**: Implement PCM fade-in/out for play/pause/stop/seek transitions
6. **Output switching**: Debounced device change with smooth transition
7. **Error recovery**: Separate XRUN recovery from device-level recovery
8. **Diagnostic logging**: Add structured `PlayerLogger::diagnostic()` events

### P2 — Nice to Have
9. **Artifact monitoring**: Port `AudioArtifactMonitor` integration
10. **Position anomaly detection**: Port stall/jump/regression detection
11. **Hot reconfigure**: Change format without full pipeline rebuild
12. **Render mirror**: Debug capture of submitted PCM

---

## Code Quality Notes

- **Clean separation**: Output worker, format negotiator, and player state are well-separated
- **Consistent patterns**: Follows same `startPipeline`/`teardownPipeline` pattern as WASAPI
- **Thread safety**: Uses Qt signal/slot for cross-thread communication
- **Error handling**: Basic but functional XRUN and device error handling
- **Missing**: No `stabilityMode`, no `creativeChannelReorder`, no `exactPlayback` effect beyond rate selection
