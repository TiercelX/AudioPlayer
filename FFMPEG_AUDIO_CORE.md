# FFmpeg Audio-Core Profile

This project currently targets a slim `audio-core` FFmpeg 9.0.1 profile for distribution builds.

## Guaranteed playback scope

The `audio-core` profile should guarantee runtime support for:

- `wav` / PCM
- `flac`
- `mp3`
- `aac` elementary streams (`.aac`, ADTS)
- `m4a` / AAC-in-MP4 audio files
- `alac` / ALAC-in-M4A audio files
- `ac3`
- `eac3` / Dolby Digital Plus
- raw `ec3` / `eb3`
- `truehd` / raw `mlp`
- Matroska audio files used by this app for sidecar remux (`.mka`)

The playback pipeline also relies on:

- PCM output to stdout
- file and pipe I/O
- `aresample` and `aformat` audio filters
- stream copy remux into Matroska for raw Dolby sidecars

## Best-effort or out-of-scope formats

These are not part of the guaranteed `audio-core` distribution profile:

- `ogg` / Vorbis / Opus
- `ape`
- `wavpack`
- `dsd`
- video containers and video decode/render paths

They may still work with larger FFmpeg builds, but the project should not promise them in the slim profile until they are explicitly covered by automated regression.

## Distribution guidance

- Default app builds must bundle the self-built `runtime-with-ffprobe` audio-core
  runtime, not a full upstream tool bundle.
- The audio-core root must contain all of these files:
  - `bin\ffmpeg.exe`
  - `bin\ffprobe.exe`
  - `include\libavformat\avformat.h`
  - `lib\avformat.lib`
  - `lib\avcodec.lib`
  - `lib\avutil.lib`
  - `lib\swresample.lib`
- Both bundled tools must report FFmpeg `9.0.1` (the tag build may print
  `n9.0.1`); the app build rejects a stale or full-build executable under the
  audio-core root.
- `scripts/build-app.ps1` fails before CMake configure when that file set is
  incomplete. Pass `-FfmpegAudioCoreRoot <root>` only to point at another
  complete self-built audio-core root.
- CMake also enforces the same rule on Windows through
  `AUDIOPLAYER_REQUIRE_FFMPEG_AUDIO_CORE=ON` by default. Set it to `OFF` only
  for a controlled diagnostic build that intentionally disables the bundled
  audio-core contract.
- CMake cache variables still name the deploy-time tools, but default builds
  require them to resolve to the same audio-core root:
  - `AUDIOPLAYER_DEPLOY_FFMPEG_EXECUTABLE`
  - `AUDIOPLAYER_DEPLOY_FFPROBE_EXECUTABLE`
- Leave them empty to use
  `AUDIOPLAYER_FFMPEG_AUDIO_CORE_ROOT\bin\ffmpeg.exe` and `ffprobe.exe`.
- The checked-in `scripts/build-app.ps1` no longer accepts
  `-DeployFfmpegExecutable` or `-DeployFfprobeExecutable` as a fallback to
  arbitrary external tools.
- Runtime probing still has code paths for missing `ffprobe`, but the default
  packaging path must ship the slim self-built `ffprobe.exe`.
- Runtime tool lookup uses this order independently for `ffmpeg` and `ffprobe`:
  1. executable beside the app (`ffmpeg.exe`/`ffprobe.exe`, or Unix names);
  2. the file or directory named by `AUDIOPLAYER_FFMPEG_PATH` or
     `AUDIOPLAYER_FFPROBE_PATH`;
  3. the corresponding executable on the global `PATH`.
  A path override may therefore point at a shared FFmpeg `bin` directory. The
  global tools are fallback-only and are never copied into the package.
- The deploy step also prunes Qt Multimedia's bundled FFmpeg backend files from the playable package by default, because this app uses its own external `ffmpeg + QAudioSink` playback path.

## Runtime deploy profile

The current app runtime only needs one slim `ffmpeg` binary. A minimal `runtime-deploy` profile should keep:

- Protocols:
  - `file`
  - `pipe`
- Demuxers:
  - `aac`
  - `ac3`
  - `eac3`
  - `flac`
  - `matroska`
  - `mov`
  - `mp3`
  - `truehd`
  - `wav`
- Muxers:
  - `matroska`
  - `null`
  - `pcm_f32le`
  - `pcm_s16le`
  - `pcm_s32le`
  - `pcm_u8`
- Decoders:
  - `aac`
  - `ac3`
  - `alac`
  - `eac3`
  - `flac`
  - `mp3`
  - `mp3float`
  - `pcm_f32le`
  - `pcm_s16le`
  - `pcm_s24le`
  - `pcm_s32le`
  - `pcm_u8`
  - `truehd`
- Encoders:
  - `pcm_f32le`
  - `pcm_s16le`
  - `pcm_s32le`
  - `pcm_u8`
- Filters:
  - `aformat`
  - `aresample`
  - `channelmap`
  - `pan`
- Parsers:
  - `aac`
  - `ac3`
  - `flac`
  - `mlp`
  - `mpegaudio`

This profile matches the current app behavior:

- raw `eb3/ec3/mlp` probing and sidecar remux
- `mka` sidecar output
- `ffmpeg`-only metadata fallback using `-f null -`
- realtime PCM output to stdout for `QAudioSink`

Note:
- The configure-time muxer modules are `pcm_*`.
- The runtime `ffmpeg -f` names used by the app remain `f32le`, `s16le`, `s32le`, and `u8`.

## Fixture generation

The checked-in playback regression fixtures still rely on a larger host-side `ffmpeg` than the deploy profile.

- The runtime package does not need AAC/ALAC/AC3/EAC3 encoders just to run the app.
- The fixture script currently uses the host toolchain to generate `wav/flac/mp3/aac/m4a/alac/ac3/ec3` samples.
- Do not inflate the deploy profile just to satisfy fixture generation.

If you later want one custom FFmpeg build to cover both deploy and fixture generation, that is a separate `dev/fixture` profile and should explicitly add the needed encoders and the `pan` filter.

## Build skeleton

Use [scripts/build-ffmpeg-audio-core.ps1](/K:/Qt/AudioPlayer/scripts/build-ffmpeg-audio-core.ps1:1) to generate a reproducible shell build script for the current `runtime-deploy` profile. It writes a POSIX shell script with the exact configure flags instead of relying on a hand-maintained command line.

For native Windows builds, the same script also supports `-Toolchain msvc -RunBuild`. The checked-in flow assumes:

- Visual Studio Build Tools are installed and `vcvars64.bat` is available.
- `MSYS2` is installed at `C:\msys64`.
- `make`, `nasm`, and `pkgconf` are installed in the MSYS2 environment.

The PowerShell wrapper imports the MSVC environment, generates MSYS2 wrapper scripts for `cl/link/lib/dumpbin`, and then invokes the generated FFmpeg shell script through `msys2_shell.cmd`.

When `-RunBuild` is used:

- The script checks that `make`, `nasm`, and `pkgconf` are available in MSYS2 before starting a long build.
- It writes a small build stamp under the target prefix.
- If the generated profile script has not changed and the expected output binaries already exist, the script prints `buildSkipped:<prefix>` and avoids a full rebuild.
- Use `-ForceRebuild` to bypass that skip logic.

## Build examples

Build the self-built audio-core runtime, then build the app:

```powershell
.\scripts\build-ffmpeg-audio-core.ps1 `
  -Profile runtime-with-ffprobe `
  -Toolchain msvc `
  -RunBuild

.\scripts\build-app.ps1 -BuildDir build-mm -Configuration Release
```

If the runtime lives outside the app build directory, point the app build at the
complete self-built root:

```powershell
.\scripts\build-app.ps1 -BuildDir build-mm -Configuration Release `
  -FfmpegAudioCoreRoot C:\toolchains\ffmpeg-audio-core\runtime-with-ffprobe-msvc
```

Example skeleton generation:

```powershell
.\scripts\build-ffmpeg-audio-core.ps1 `
  -Profile runtime-deploy `
  -OutputScript build-mm\ffmpeg-audio-core-runtime.sh
```

Example native MSVC build:

```powershell
.\scripts\build-ffmpeg-audio-core.ps1 `
  -Profile runtime-deploy `
  -Toolchain msvc `
  -RunBuild `
  -DisableX86Asm
```

Example native MSVC build with slim `ffprobe` included:

```powershell
.\scripts\build-ffmpeg-audio-core.ps1 `
  -Profile runtime-with-ffprobe `
  -Toolchain msvc `
  -RunBuild `
  -DisableX86Asm
```
