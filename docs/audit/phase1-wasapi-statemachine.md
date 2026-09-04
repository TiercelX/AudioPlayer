# Phase 1: WASAPI 状态机

## 1. PlaybackState 状态转换

```
Stopped ──play()──▶ Playing
Playing ──pause()──▶ Paused
Playing ──stop()──▶ Stopping
Paused ──play()──▶ Playing (resume)
Paused ──stop()──▶ Stopping
Stopping ──decoderFinished──▶ Stopped
Playing ──error/finalize──▶ Stopped
```

| 源状态 | 目标状态 | 触发条件 | 执行操作 |
|--------|----------|----------|----------|
| Stopped | Playing | `play()` 且有 source | `startPipeline(startPositionMs, NormalStart)` |
| Playing | Paused | `pause()` | invokeMethod(worker->pauseOutput), setPlaybackState(Paused) |
| Playing | Stopping | `stop()` | releaseOutputResources(true), quarantineBuffer, stopDecoderWorker(false) |
| Paused | Playing | `play()` (resume) | invokeMethod(worker->resumeOutput), resetAnomalyTracking |
| Paused | Stopped | `stop()` 无 activeSession | clearBufferDevice, setPlaybackState(Stopped) |
| Stopping | Stopped | `handleDecoderFinished(stoppingSessionId)` | clearBufferDevice |
| Playing | Stopped | 解码/输出错误不可恢复 | teardownPipeline, emit errorOccurred |
| Playing | Stopped | `finalizePlayback()` (EOS + idle) | teardownPipeline, emit finished |

关键代码位置:
- `play()`: `windowswasapiaudioplayer.cpp:329-363`
- `pause()`: `windowswasapiaudioplayer.cpp:365-382`
- `stop()`: `windowswasapiaudioplayer.cpp:384-423`
- `finalizePlayback()`: `windowswasapiaudioplayer_state.cpp:487-500`

---

## 2. ActiveOutputSwitchPhase 状态机

WASAPI 后端独有的复杂事务机制，用于处理输出设备切换、系统设备变更、格式变更、强制刷新。

```
Idle
 │
 ▼ (beginActiveOutputSwitch)
Pending
 │
 ├──(SystemDeviceChange + Playing)──▶ OutputSuspended
 │     │                                    │
 │     ▼                                    ▼
 │   [releaseOutputResources]            Preflight
 │     │                                    │
 │     ▼                                    ▼
 │   OutputSuspended                     Applying
 │                                          │
 │   WaitingForInvalidation ◀───────────────┤
 │     │                                    │
 │     ▼                                    ▼
 │   [applyActiveOutputSwitch]      WaitingForOutputStart
 │                                          │
 │                                          ├──(ActiveState)──▶ Idle
 │                                          ├──(Error)──▶ [retry/rebuild]
 │                                          └──(exhausted)──▶ Stopped
```

| 源 Phase | 目标 Phase | 触发条件 | 执行操作 |
|----------|-----------|----------|----------|
| Idle | Pending | `beginActiveOutputSwitch(trigger)` | 创建事务, 记录 sessionId |
| Pending | OutputSuspended | trigger=SystemDeviceChange + Playing | `releaseOutputResources` → m_audioStarted=false |
| Pending | Preflight | debounce timer fired | 检查 source/状态 |
| OutputSuspended | Preflight | `applyActiveOutputSwitch()` | 检查设备/格式变化 |
| WaitingForInvalidation | Preflight | observed invalidation | quarantineBuffer, applyActiveOutputSwitch |
| Preflight | Applying | 进入 applyActiveOutputSwitch 主逻辑 | 检查设备/格式变化 |
| Applying | WaitingForOutputStart | hot-reconfigure 或 rebuild 完成 | reconfigureActiveOutput 或 startPipeline |
| WaitingForOutputStart | Idle | `handleAudioStateChanged(ActiveState)` | resetActiveOutputSwitch |
| 任意非 Idle | Idle | `resetActiveOutputSwitch(reason)` | 清空事务 |

关键代码位置:
- `beginActiveOutputSwitch()`: `windowswasapiaudioplayer_output.cpp:916-1017`
- `scheduleActiveOutputSwitch()`: `windowswasapiaudioplayer_output.cpp:1019-1060`
- `applyActiveOutputSwitch()`: `windowswasapiaudioplayer_output.cpp:1147-1332`
- `handleAudioStateChanged()`: `windowswasapiaudioplayer_state.cpp:606-918`

---

## 3. PipelineStartupProfile 启动路径

| Profile | 触发场景 | 特殊行为 |
|---------|----------|----------|
| NormalStart | `play()` from Stopped | 源切换时标记 SourceSwitch |
| SeekRestart | seek while Playing (旧路径) | 已不常用 |
| SeekResume | `seek()` while Playing | 使用 kSeekResume* 常量, 独立的启动阈值 |
| ActiveSwitchRebuild | active output switch conservative-rebuild | 使用 activeSwitchBoundaryPolicy |
| ErrorRecovery | `scheduleOutputRecovery` timer fired | 带 recoveryAttempt 计数 |

---

## 4. 错误恢复状态机

```
Playing + StoppedState + IOError/FatalError
    │
    ▼
shouldAttemptOutputRecovery() = true?
    │
    ├── Yes ──▶ scheduleOutputRecovery(sessionId, error)
    │              │
    │              ▼
    │         m_outputRecoveryPending = true
    │         m_outputRecoveryAttempt++
    │         delay = {1:250ms, 2:500ms, 3:900ms}
    │              │
    │              ▼ (QTimer::singleShot)
    │         startPipeline(positionMs, ErrorRecovery)
    │              │
    │              ├── 成功 (position stable) ──▶ resetOutputRecoveryState
    │              └── 再次失败 ──▶ 再次 scheduleOutputRecovery
    │                                  │
    │                                  └── attempt > 3 ──▶ exhausted
    │                                                        │
    │                                                        ▼
    │                                                   teardownPipeline
    │                                                   setPlaybackState(Stopped)
    │                                                   emit errorOccurred
    │
    └── No (fatal) ──▶ teardownPipeline
                       setPlaybackState(Stopped)
                       emit errorOccurred
```

关键常量:
- Recovery attempt 1: 250ms delay
- Recovery attempt 2: 500ms delay
- Recovery attempt 3: 900ms delay
- kRecoveryStablePositionAdvanceMs: 120ms (position must advance to confirm recovery)

---

## 5. 设备失效处理流程

```
系统设备变化 (QMediaDevices::audioOutputsChanged)
    │
    ▼
handleAudioOutputsChanged()
    ├── 检查 selected device 是否仍然存在
    │   └── 不存在 → fallback to default
    ├── 检查 active switch 是否已在等待
    │   └── 是 + target device == active device → return (defer)
    ├── 检查 effective output 是否实际变化
    │   └── 未变化 → return (ignored)
    └── beginActiveOutputSwitch(SystemDeviceChange, force=true)

WASAPI 输出错误 (StoppedState + Error)
    │
    ├── activeOutputSwitch 进行中?
    │   └── 是 → 吸收错误 (++absorbedOutputErrorCount)
    │       ├── phase=WaitingForInvalidation → apply immediately
    │       ├── phase=WaitingForOutputStart + retry < limit → retry
    │       └── 超限 → resetActiveOutputSwitch, teardown, Stopped
    │
    └── 无 active switch → scheduleOutputRecovery / fatal
```
