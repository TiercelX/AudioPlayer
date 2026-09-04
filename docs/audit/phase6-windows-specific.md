# Phase 6 Audit: Windows-Specific Code Analysis

> Generated: 2026-06-13
> Scope: All Windows-specific code requiring changes for Linux/macOS porting

## Summary

The project has three major Windows-only code areas:
1. **WASAPI backend** (`src/backends/wasapi/`) — ~2800 lines across 7 files
2. **ASIO backend** (`src/backends/asio/`) — ~2500 lines across 11 files
3. **Platform guards** in UI/core — ~30 `#if defined(Q_OS_WINDOWS)` blocks

The ALSA backend already exists for Linux. macOS (CoreAudio) has no implementation yet.

---

## 1. WASAPI Backend (`src/backends/wasapi/`)

### 1.1 Windows API Headers

| File:Line | Code | Linux/macOS Equivalent | Difficulty |
|-----------|------|----------------------|------------|
| `windowswasapiaudioplayer.cpp:27` | `#include <QWinEventNotifier>` | `QSocketNotifier` or poll-based equivalent | Medium |
| `windowswasapiaudioplayer.cpp:34-37` | `#include <audioclient.h>`, `<ksmedia.h>`, `<mmdeviceapi.h>`, `<objbase.h>` | ALSA (`<alsa/asoundlib.h>`) / CoreAudio (`<AudioToolbox/AudioToolbox.h>`) | N/A (replaced) |
| `windowswasapiaudioplayer_worker.h:29` | `#include <QWinEventNotifier>` | Same as above | Medium |
| `windowswasapiaudioplayer_worker.h:38-41` | Same Windows audio headers | Same as above | N/A |
| `windowswasapiaudioplayer_worker_helpers.h:29` | `#include <QWinEventNotifier>` | Same as above | Medium |
| `windowswasapiaudioplayer_worker_helpers.h:36-39` | Same Windows audio headers | Same as above | N/A |

**Note**: The entire WASAPI backend is Windows-only by design. It does NOT need to be ported — the ALSA backend serves the same role on Linux. The key issue is ensuring the WASAPI files are conditionally compiled out on non-Windows.

### 1.2 COM Initialization

| File:Line | Code | Linux/macOS Equivalent | Difficulty |
|-----------|------|----------------------|------------|
| `windowswasapiaudioplayer_output.cpp:57-79` | `ScopedComInitializer` class using `CoInitializeEx` / `CoUninitialize` | Not needed on Linux/macOS | N/A |
| `windowswasapiaudioplayer_worker.h:86-88` | `CoUninitialize()` in destructor | Not needed | N/A |
| `windowswasapiaudioplayer_worker_helpers.h:211-213` | `ensureComInitialized()` | Not needed | N/A |

### 1.3 WASAPI-Specific Types and APIs

| File:Line | Code | Linux/macOS Equivalent | Difficulty |
|-----------|------|----------------------|------------|
| `windowswasapiaudioplayer_output.cpp:91-119` | `channelMaskForCount()` using `SPEAKER_*` constants | ALSA channel maps / CoreAudio `AudioChannelLayout` | N/A (backend-specific) |
| `windowswasapiaudioplayer_output.cpp:122-130` | `channelMaskFromWaveFormat()` using `WAVEFORMATEXTENSIBLE` | ALSA has no equivalent concept | N/A |
| `windowswasapiaudioplayer_output.cpp:186-203` | `waveFormatSize()`, `copyWaveFormat()` | ALSA uses `snd_pcm_hw_params` | N/A |
| `windowswasapiaudioplayer_output.cpp:205-256` | `sampleEncodingFromWaveFormat()` | ALSA: `snd_pcm_format_t` mapping | N/A |
| `windowswasapiaudioplayer_output.cpp:338-379` | `buildWaveFormat()` using `WAVEFORMATEXTENSIBLE` | ALSA: `snd_pcm_hw_params_set_format` | N/A |
| `windowswasapiaudioplayer_output.cpp:602-659` | `openRenderEndpoint()` using `IMMDeviceEnumerator`, `IAudioClient` | ALSA: `snd_pcm_open` | N/A |
| `windowswasapiaudioplayer_worker.h:805-811` | `IMMDevice*`, `IAudioClient*`, `IAudioRenderClient*`, `IAudioStreamVolume*`, `HANDLE m_refillEvent` | ALSA: `snd_pcm_t*` | N/A |
| `windowswasapiaudioplayer_worker_helpers.h:80` | `REFERENCE_TIME kExclusiveBufferDuration` | ALSA: `snd_pcm_hw_params_set_buffer_size` | N/A |
| `windowswasapiaudioplayer_worker_helpers.h:586-600` | `mapWasapiError()` using `AUDCLNT_E_*` HRESULT codes | ALSA: `snd_strerror()` error codes | N/A |

### 1.4 WASAPI Worker Event-Driven Model

| File:Line | Code | Linux/macOS Equivalent | Difficulty |
|-----------|------|----------------------|------------|
| `windowswasapiaudioplayer_worker.h:809` | `HANDLE m_refillEvent` | ALSA uses polling (`snd_pcm_wait`) | N/A |
| `windowswasapiaudioplayer_worker.h:810` | `QWinEventNotifier *m_eventNotifier` | `QSocketNotifier` with ALSA fd | N/A |

### 1.5 Spatial Audio / Endpoint Flush

| File:Line | Code | Linux/macOS Equivalent | Difficulty |
|-----------|------|----------------------|------------|
| `windowswasapiaudioplayer.cpp:592-677` | `spatialEndpointFlushEnabled()`, `performSpatialEndpointFlush()` | No equivalent on Linux; skip on macOS | N/A |

### 1.6 Exclusive Mode

| File:Line | Code | Linux/macOS Equivalent | Difficulty |
|-----------|------|----------------------|------------|
| `windowswasapiaudioplayer_worker_helpers.h:80` | `kExclusiveBufferDuration = 1000000` (0.1s) | ALSA hw: exclusive mode / CoreAudio hog mode | N/A |

### 1.7 Creative Channel Reorder

| File:Line | Code | Linux/macOS Equivalent | Difficulty |
|-----------|------|----------------------|------------|
| `windowswasapiaudioplayer_worker_helpers.h:161-207` | `isCreativeG5WasapiDeviceDescription()`, `creativeWasapiChannelOrderFilter()` | Not applicable; ALSA handles channel maps natively | N/A |

---

## 2. ASIO Backend (`src/backends/asio/`)

### 2.1 Windows-Only Dependencies

| File:Line | Code | Linux/macOS Equivalent | Difficulty |
|-----------|------|----------------------|------------|
| `windowsasioaudioplayer.cpp:42-53` | `#include <windows.h>`, `<objbase.h>`, `<audioclient.h>`, `<audiopolicy.h>`, `<endpointvolume.h>`, `<mmdeviceapi.h>`, `<propidl.h>`, `<propsys.h>` | No equivalent; ASIO is Windows-only | N/A |
| `windowsasioaudioplayer.cpp:57-60` | `PKEY_AudioEndpoint_FriendlyName` PROPERTYKEY | No equivalent | N/A |
| `asio_interface.h` | ASIO SDK interface | No equivalent on Linux/macOS | N/A |

**Note**: ASIO is a Windows-only professional audio driver interface. It has no Linux or macOS equivalent. The ASIO backend should be completely excluded on non-Windows platforms via CMake.

### 2.2 ASIO Discovery and Probe

| File:Line | Code | Linux/macOS Equivalent | Difficulty |
|-----------|------|----------------------|------------|
| `windowsasioaudioplayer_discovery.cpp/h` | Windows registry-based ASIO driver enumeration | Not applicable | N/A |
| `windowsasioaudioplayer_sessionprobe.cpp/h` | `AsioSessionProbe::detectMultiplePhysicalDevicesForAsioDriver()`, `resolveWasapiEndpointForAsioDriver()` | Not applicable | N/A |

---

## 3. Platform Guards in UI Layer

### 3.1 mainwindow.cpp

| File:Line | Code | Linux/macOS Equivalent | Difficulty |
|-----------|------|----------------------|------------|
| `mainwindow.cpp:37-39` | `#include "windowsasioaudioplayer.h"` | Guard with `#if defined(Q_OS_WINDOWS)` (already done) | Done |
| `mainwindow.cpp:76-78` | `WindowsAsioAudioPlayer::setHostWindowHandle(winId())` | Guard with `#if defined(Q_OS_WINDOWS)` (already done) | Done |
| `mainwindow.cpp:399-510` | ASIO error handling, ASIO retry timer logic | Guard with `#if defined(Q_OS_WINDOWS)` (already done) | Done |

### 3.2 mainwindow_output.cpp

| File:Line | Code | Linux/macOS Equivalent | Difficulty |
|-----------|------|----------------------|------------|
| `mainwindow_output.cpp:16-18` | `#include "windowsasioaudioplayer.h"`, `"windowsasioaudioplayer_sessionprobe.h"` | Guard (already done) | Done |
| `mainwindow_output.cpp:62-63` | `WindowsAsioAudioPlayer::setHostWindowHandle(winId())` | Guard (already done) | Done |
| `mainwindow_output.cpp:114-153` | ASIO device enumeration in output device menu | Guard (already done); Linux/macOS would list ALSA/CoreAudio devices instead | Done |
| `mainwindow_output.cpp:237-260` | ASIO multi-device detection warning | Guard (already done) | Done |
| `mainwindow_output.cpp:329-395` | WASAPI exclusive ↔ ASIO transition logic | Guard (already done) | Done |
| `mainwindow_output.cpp:482-573` | `availableAsioOutputDevices()`, `selectAsioOutputDeviceByIndex()` | Guard (already done) | Done |

### 3.3 mainwindow_helpers.cpp

| File:Line | Code | Linux/macOS Equivalent | Difficulty |
|-----------|------|----------------------|------------|
| `mainwindow_helpers.cpp:12` | `#if defined(Q_OS_WINDOWS)` include guard | Already done | Done |

### 3.4 mainwindow_media.cpp

| File:Line | Code | Linux/macOS Equivalent | Difficulty |
|-----------|------|----------------------|------------|
| `mainwindow_media.cpp:19` | `#if defined(Q_OS_WINDOWS)` include guard | Already done | Done |
| `mainwindow_media.cpp:214` | `#if defined(Q_OS_WINDOWS)` block | Already done | Done |

### 3.5 audioplayerfactory.cpp

| File:Line | Code | Linux/macOS Equivalent | Difficulty |
|-----------|------|----------------------|------------|
| `audioplayerfactory.cpp:7-10` | `#if defined(Q_OS_WINDOWS)` includes for WASAPI/ASIO | Already guarded | Done |
| `audioplayerfactory.cpp:66-70` | Default backend selection: Windows→WASAPI, others→FFmpeg | Already guarded; Linux selects ALSA at line 119 | Done |
| `audioplayerfactory.cpp:119-137` | `#elif defined(Q_OS_LINUX)` ALSA selection | Already implemented | Done |
| `audioplayerfactory.cpp:177-187` | Factory `create()` with `#if defined(Q_OS_WINDOWS)` | Already guarded | Done |
| `audioplayerfactory.cpp:193-197` | `#if defined(Q_OS_LINUX)` ALSA creation | Already implemented | Done |

### 3.6 automationoptions.cpp

| File:Line | Code | Linux/macOS Equivalent | Difficulty |
|-----------|------|----------------------|------------|
| `automationoptions.cpp:6,21,63` | `#if defined(Q_OS_WINDOWS)` guards | Already guarded | Done |

### 3.7 Other Files

| File:Line | Code | Linux/macOS Equivalent | Difficulty |
|-----------|------|----------------------|------------|
| `pcmseekcache.cpp:10,24` | `#ifdef Q_OS_WIN` | Already guarded | Done |
| `ffmpegaudioplayer_state.cpp:139,168` | `#ifdef Q_OS_WINDOWS` / `#ifndef Q_OS_WINDOWS` | Already guarded | Done |

---

## 4. Missing Platform Support

### 4.1 macOS (CoreAudio) — Not Implemented

| Component | Status | Required Work |
|-----------|--------|--------------|
| CoreAudio backend | Not started | New `src/backends/coreaudio/` directory with ~1500-2000 lines |
| Device enumeration | Not started | Use `AudioObjectGetPropertyData` with `kAudioHardwarePropertyDevices` |
| Exclusive mode (hog mode) | Not started | `kAudioDevicePropertyHogMode` |
| Format negotiation | Not started | `kAudioStreamPropertyPhysicalFormat` |
| Output worker | Not started | `AudioUnit` render callback or `AudioQueue` |

### 4.2 CMake Platform Conditions

The CMakeLists.txt needs review to ensure:
- WASAPI/ASIO sources are excluded on non-Windows
- ALSA sources are excluded on non-Linux
- CoreAudio sources would be excluded on non-macOS
- `AUDIOPLAYER_LIBAV_DECODER` define is platform-independent

---

## 5. Porting Effort Summary

| Area | Windows Lines | Linux Status | macOS Status | Effort to Port |
|------|--------------|-------------|-------------|----------------|
| WASAPI backend | ~2800 | N/A (ALSA exists) | N/A (CoreAudio needed) | 0 (Linux), ~2000 (macOS) |
| ASIO backend | ~2500 | N/A (excluded) | N/A (excluded) | 0 |
| UI platform guards | ~30 guards | All guarded | All guarded | 0 |
| Factory routing | ~50 lines | Implemented | Needs CoreAudio case | ~20 lines |
| CoreAudio backend | 0 | N/A | Not started | ~2000 lines |
| CMake platform config | Unknown | Needs review | Needs review | ~50 lines |

**Total estimated porting work:**
- **Linux**: Minimal (ALSA backend exists, guards are in place)
- **macOS**: ~2000+ lines for new CoreAudio backend + CMake changes
