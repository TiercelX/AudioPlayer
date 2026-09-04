# MiMo ASIO Handoff - 2026-05-24 (session 2)

## 工作目录

- Worktree: `D:\AI\Codex\AudioPlayer-codex-0523-asio`
- Branch: `codex-0523-asio`（dirty，未提交）

## 本次对话做了什么

## Codex cleanup follow-up

Codex reviewed this state after the MiMo session and cleaned up the unreliable
parts instead of preserving the ambiguous dirty tree:

- Removed the unused `WindowsAsioProcessAudioPlayer` files and CMake entries.
- Removed the hidden `--asio-host-play` path; the standalone
  `--asio-probe-driver` diagnostic path remains available.
- Hardened the ASIO busy retry lifecycle with a generation token so old retry
  timers cannot restart playback after stop, source change, or device change.
- Busy retry no longer reports `Playing` while only waiting for another app to
  release the endpoint; first `bufferSwitch` remains the ASIO start evidence.

This handoff is retained only as investigation context. The current source of
truth for implemented behavior and validation is `docs/bug/asio-status.md`.

### 1. 发现 isolated host 是根因

Codex 加的 `WindowsAsioProcessAudioPlayer`（isolated host 包装）导致 Creative ASIO 驱动在子进程里 `IASIO::init` 必崩。子进程从 UI 进程继承了 COM/线程状态，污染了驱动。

证据：
- 非 isolated 路径（`WindowsAsioAudioPlayer` 直接在 UI 进程）：`reportResult:PASS`，能听到测试音
- Isolated host 路径：`crashed=1`，所有重试都失败
- 手动 child probe（独立进程）null-handle：`result=1 crashed=0` 成功过

### 2. Factory 改动

`audioplayerfactory.cpp` 第 161 行：
```cpp
// Codex 改的（broken）：
return new WindowsAsioProcessAudioPlayer(parent);
// 恢复为：
return new WindowsAsioAudioPlayer(parent);
```

### 3. Session check 验证通过

`hasActiveExternalWasapiRenderSessionsForAsioDriver` 能正确检测 Sound Blaster 端点上的外部 WASAPI 会话，阻止 ASIO init。Occupied 测试：`Session blocked: True`。

### 4. 尝试的方案（全部回退了）

以下改动在 `windowsasioaudioplayer.cpp` 里试过，**已全部 `git checkout` 回退到 Codex 版本**：
- WASAPI shared-mode preflight（`runWasapiSharedModePreflight`）
- DLL unload（`unloadAsioDriverDll`）
- Creative 重试次数增加（1→3）
- 跳过 child probe，直接 init
- 跳过 WASAPI exclusive preflight
- Post-preflight cooldown

这些方案都没解决根本问题。

### 5. 发现的真正问题

不是"WASAPI 占用毒化 ASIO"，而是 **"上一次 ASIO 会话结束后驱动进入坏状态"**：
- 第一次 ASIO 播放：成功
- ASIO 会话结束（app 退出）：驱动被毒化
- 下一次 ASIO init：`crashed=1`
- 需要重启电脑才能恢复

用户的实际场景（Apple Music 关掉 → ASIO 播放）应该能工作，因为之前没有 ASIO 会话。

### 6. ffmpeg 问题

Build 脚本之前用全量版 `D:\Tool\ffmpeg\bin\ffmpeg.exe`（214MB）。用户要求用自编译 audio-core 版本。

已设置环境变量 `MSYS2_ROOT=D:\msys64`。还需要：
1. Clone FFmpeg 源码到 `build-mimo-asio/ffmpeg-src`
2. 运行 `scripts\build-ffmpeg-audio-core.ps1 -Toolchain msvc -RunBuild`

## MiMo session-end code state before Codex cleanup

### 已修改的文件
- `src/core/audioplayerfactory.cpp` — factory 改回 `WindowsAsioAudioPlayer`
- `docs/bug/asio-status.md` — 更新了状态文档（含错误的 isolated host 分析）

### 未修改的文件（Codex 原版）
- `src/backends/asio/windowsasioaudioplayer.cpp` — 完全回退到 Codex 版本
- `src/backends/asio/windowsasioaudioplayer.h`
- `src/backends/asio/windowsasioprocessaudioplayer.cpp` — 当时未跟踪，后续 Codex cleanup 已删除
- `src/backends/asio/windowsasioprocessaudioplayer.h` — 当时未跟踪，后续 Codex cleanup 已删除
- `CMakeLists.txt`
- `src/ui/main.cpp`
- `src/ui/mainwindow.cpp`

### Build 脚本
- `build-mimo-asio.cmd` — 去掉了全量 ffmpeg 路径，用默认 resolution

## 下一步

1. **Clone FFmpeg 源码**：`git clone --depth 1 --branch n7.1 https://github.com/FFmpeg/FFmpeg.git build-mimo-asio\ffmpeg-src`
2. **Build audio-core ffmpeg**：`scripts\build-ffmpeg-audio-core.ps1 -Toolchain msvc -RunBuild`
3. **重启电脑**恢复驱动状态
4. **验证热切**：先用 WASAPI shared 播放，再切 ASIO，确认能恢复
5. **清理**：`windowsasioprocessaudioplayer.cpp/.h` 可以删除（isolated host 不再使用；后续 Codex cleanup 已完成）

## 需要记住的

- `MSYS2_ROOT=D:\msys64`（已设为系统环境变量）
- MSYS2 路径是 `D:\msys64`，不是 `D:\MSYS2` 或 `C:\MSYS2`
- 不要用全量 ffmpeg，始终用 audio-core 自编译版
- `build-mimo-asio.cmd` 不要加 `-DeployFfmpegExecutable` 参数
