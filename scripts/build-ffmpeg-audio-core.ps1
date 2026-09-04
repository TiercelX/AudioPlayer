param(
    [ValidateSet("runtime-deploy", "runtime-with-ffprobe")]
    [string]$Profile = "runtime-with-ffprobe",
    [ValidateSet("generic", "msvc")]
    [string]$Toolchain = "generic",
    [string]$OutputScript = "",
    [string]$SourceDirPlaceholder = "/path/to/ffmpeg-source",
    [string]$PrefixDirPlaceholder = "/path/to/ffmpeg-audio-core",
    [string]$SourceDir = "",
    [string]$PrefixDir = "",
    [int]$Jobs = 4,
    [string]$VcVars64Path = "",
    [string]$Msys2ShellPath = "",
    [switch]$DisableX86Asm,
    [switch]$ForceRebuild,
    [switch]$RunBuild,
    [switch]$PrintScript
)

$ErrorActionPreference = "Stop"
$script:FfmpegVersion = "9.0.1"

function Convert-ToBashPath {
    param(
        [string]$Path
    )

    $resolvedPath = [System.IO.Path]::GetFullPath($Path)
    if ($resolvedPath -match '^[A-Za-z]:\\') {
        $drive = $resolvedPath.Substring(0, 1).ToLowerInvariant()
        $rest = $resolvedPath.Substring(2) -replace '\\', '/'
        return "/$drive$rest"
    }

    return $resolvedPath -replace '\\', '/'
}

function Get-EnvironmentFromBatchFile {
    param(
        [string]$BatchFile
    )

    $cmdLine = '"' + $BatchFile + '" >nul && set'
    $envLines = & cmd.exe /c $cmdLine
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to load environment from batch file: $BatchFile"
    }

    $envMap = @{}
    foreach ($line in $envLines) {
        if ($line -match '^(.*?)=(.*)$') {
            $key = $matches[1]
            $value = $matches[2]
            if ($key -ieq 'Path' -and $envMap.ContainsKey('PATH') -and $key -cne 'PATH') {
                continue
            }
            $envMap[$key] = $value
        }
    }

    return $envMap
}

function Join-ConfigureList {
    param(
        [string]$Prefix,
        [string[]]$Values
    )

    if ($null -eq $Values -or $Values.Count -eq 0) {
        return @()
    }

    return @("$Prefix=$($Values -join ',')")
}

function Write-Msys2Wrapper {
    param(
        [string]$WrapperPath,
        [string[]]$Lines
    )

    Set-Content -Path $WrapperPath -Value $Lines -Encoding ascii
}

function Write-Msys2MsvcToolWrappers {
    param(
        [string]$WrapperDirectory,
        [string]$CcPath,
        [string]$LdPath,
        [string]$ArPath,
        [string]$NmPath
    )

    $null = New-Item -ItemType Directory -Force -Path $WrapperDirectory

    Write-Msys2Wrapper -WrapperPath (Join-Path $WrapperDirectory "cl.exe") -Lines @(
        '#!/usr/bin/env bash',
        'set -euo pipefail',
        'export MSYS2_ARG_CONV_EXCL="*"',
        "REAL=""$(Convert-ToBashPath -Path $CcPath)""",
        'if [[ "${1:-}" == "-nologo-" ]]; then',
        '  echo "Microsoft (R) C/C++ Optimizing Compiler Version 19.50.35729 for x64" >&2',
        '  exit 0',
        'fi',
        'preprocess=0',
        'compile_only=0',
        'output=""',
        'args=()',
        'while (($#)); do',
        '  case "$1" in',
        '    -c)',
        '      compile_only=1',
        '      args+=("/c")',
        '      shift',
        '      ;;',
        '    -E)',
        '      preprocess=1',
        '      args+=("/E")',
        '      shift',
        '      ;;',
        '    -o)',
        '      output="$2"',
        '      shift 2',
        '      ;;',
        '    -D*)',
        '      args+=("/D${1#-D}")',
        '      shift',
        '      ;;',
        '    -I*)',
        '      args+=("/I${1#-I}")',
        '      shift',
        '      ;;',
        '    -Winvalid-offsetof|-Werror=unknown-warning-option|-Werror=unused-command-line-argument|-Winvalid-pch|-Qunused-arguments)',
        '      shift',
        '      ;;',
        '    *)',
        '      args+=("$1")',
        '      shift',
        '      ;;',
        '  esac',
        'done',
        'if [[ -n "$output" ]]; then',
        '  if (( preprocess )); then',
        '    exec "$REAL" "${args[@]}" >"$output"',
        '  elif (( compile_only )); then',
        '    args+=("/Fo$output")',
        '  else',
        '    args+=("/Fe$output")',
        '  fi',
        'fi',
        'exec "$REAL" "${args[@]}"'
    )

    Write-Msys2Wrapper -WrapperPath (Join-Path $WrapperDirectory "link.exe") -Lines @(
        '#!/usr/bin/env bash',
        'set -euo pipefail',
        'export MSYS2_ARG_CONV_EXCL="*"',
        "REAL=""$(Convert-ToBashPath -Path $LdPath)""",
        'args=()',
        'while (($#)); do',
        '  case "$1" in',
        '    -o)',
        '      args+=("-out:$2")',
        '      shift 2',
        '      ;;',
        '    *)',
        '      args+=("$1")',
        '      shift',
        '      ;;',
        '  esac',
        'done',
        'exec "$REAL" "${args[@]}"'
    )

    Write-Msys2Wrapper -WrapperPath (Join-Path $WrapperDirectory "lib.exe") -Lines @(
        '#!/usr/bin/env bash',
        'export MSYS2_ARG_CONV_EXCL="*"',
        "exec ""$(Convert-ToBashPath -Path $ArPath)"" ""`$@"""
    )

    Write-Msys2Wrapper -WrapperPath (Join-Path $WrapperDirectory "dumpbin.exe") -Lines @(
        '#!/usr/bin/env bash',
        'export MSYS2_ARG_CONV_EXCL="*"',
        "exec ""$(Convert-ToBashPath -Path $NmPath)"" ""`$@"""
    )

    Write-Msys2Wrapper -WrapperPath (Join-Path $WrapperDirectory "cmp") -Lines @(
        '#!/usr/bin/env bash',
        'set -euo pipefail',
        'args=()',
        'while (($#)); do',
        '  case "$1" in',
        '    -s|--silent|--quiet|--)',
        '      shift',
        '      ;;',
        '    -*)',
        '      shift',
        '      ;;',
        '    *)',
        '      args+=("$1")',
        '      shift',
        '      ;;',
        '  esac',
        'done',
        'if ((${#args[@]} < 2)); then',
        '  echo "usage: cmp FILE1 FILE2" >&2',
        '  exit 2',
        'fi',
        'perl -e ''use strict; use warnings; my ($left, $right) = @ARGV; open my $lhs, q{<:raw}, $left or exit 2; open my $rhs, q{<:raw}, $right or exit 2; my ($lb, $rb); while (1) { my $lr = read($lhs, $lb, 8192); my $rr = read($rhs, $rb, 8192); exit 2 unless defined $lr && defined $rr; exit 0 if $lr == 0 && $rr == 0; exit 1 if $lr != $rr || $lb ne $rb; }'' -- "${args[0]}" "${args[1]}"'
    )
}

function Get-FileSha256 {
    param(
        [string]$Path
    )

    $getFileHashCommand = Get-Command Get-FileHash -ErrorAction SilentlyContinue
    if ($null -ne $getFileHashCommand) {
        return (Get-FileHash -Path $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    }

    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $sha256 = [System.Security.Cryptography.SHA256]::Create()
        try {
            $hashBytes = $sha256.ComputeHash($stream)
            return ([System.BitConverter]::ToString($hashBytes) -replace "-", "").ToLowerInvariant()
        } finally {
            $sha256.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
}

function Get-ExecutableVersionLine {
    param(
        [string]$Path
    )

    if (-not (Test-Path $Path -PathType Leaf)) {
        return ""
    }

    return [string](@(& $Path -version 2>$null) | Select-Object -First 1)
}

function Normalize-TextForComparison {
    param(
        [string]$Text
    )

    if ($null -eq $Text) {
        return ""
    }

    return $Text.TrimEnd("`r", "`n")
}

function Invoke-Msys2Command {
    param(
        [string]$Msys2ShellPath,
        [string]$Command
    )

    & $Msys2ShellPath -msys -defterm -no-start -use-full-path -here -shell bash -lc $Command
    if ($LASTEXITCODE -ne 0) {
        throw "MSYS2 command failed with exit code ${LASTEXITCODE}: $Command"
    }
}

function Resolve-VcVars64Path {
    $override = [System.Environment]::GetEnvironmentVariable("AUDIOPLAYER_VCVARS64_PATH")
    if (-not [string]::IsNullOrWhiteSpace($override)) {
        return $override
    }

    $vswhere = Get-Command vswhere.exe -ErrorAction SilentlyContinue
    if ($null -eq $vswhere) {
        $programFilesX86 = [System.Environment]::GetEnvironmentVariable("ProgramFiles(x86)")
        if (-not [string]::IsNullOrWhiteSpace($programFilesX86)) {
            $vswherePath = Join-Path $programFilesX86 "Microsoft Visual Studio\Installer\vswhere.exe"
            if (Test-Path $vswherePath -PathType Leaf) {
                $vswhere = Get-Item $vswherePath
            }
        }
    }

    if ($null -ne $vswhere) {
        $vswherePath = if (-not [string]::IsNullOrWhiteSpace($vswhere.Source)) {
            $vswhere.Source
        } else {
            $vswhere.FullName
        }
        foreach ($vswhereArgList in @(
            @("-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-find", "VC\Auxiliary\Build\vcvars64.bat"),
            @("-latest", "-products", "*", "-find", "VC\Auxiliary\Build\vcvars64.bat"),
            @("-latest", "-find", "VC\Auxiliary\Build\vcvars64.bat"),
            @("-all", "-find", "VC\Auxiliary\Build\vcvars64.bat")
        )) {
            $candidate = & $vswherePath @vswhereArgList | Select-Object -First 1
            if (-not [string]::IsNullOrWhiteSpace($candidate)) {
                return $candidate
            }
        }
    }

    $programFiles = [Environment]::GetEnvironmentVariable("ProgramFiles")
    $programFilesX86 = [Environment]::GetEnvironmentVariable("ProgramFiles(x86)")
    foreach ($root in @($programFiles, $programFilesX86, "D:\Program Files", "D:\Program Files (x86)")) {
        if ([string]::IsNullOrWhiteSpace($root)) { continue }
        $candidate = Join-Path $root "Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
        if (Test-Path $candidate -PathType Leaf) { return $candidate }
        $candidate = Join-Path $root "Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
        if (Test-Path $candidate -PathType Leaf) { return $candidate }
    }

    return ""
}

function Resolve-Msys2ShellPath {
    $override = [System.Environment]::GetEnvironmentVariable("AUDIOPLAYER_MSYS2_SHELL_PATH")
    if (-not [string]::IsNullOrWhiteSpace($override)) {
        return $override
    }

    $command = Get-Command msys2_shell.cmd -ErrorAction SilentlyContinue
    if ($null -ne $command -and -not [string]::IsNullOrWhiteSpace($command.Source)) {
        return $command.Source
    }

    $msys2Root = [System.Environment]::GetEnvironmentVariable("MSYS2_ROOT")
    if (-not [string]::IsNullOrWhiteSpace($msys2Root)) {
        $candidate = Join-Path $msys2Root "msys2_shell.cmd"
        if (Test-Path $candidate -PathType Leaf) {
            return $candidate
        }
    }

    foreach ($candidate in @("C:\msys64\msys2_shell.cmd", "D:\msys64\msys2_shell.cmd")) {
        if (Test-Path $candidate -PathType Leaf) {
            return $candidate
        }
    }

    return ""
}

$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "common-paths.ps1")
$buildDir = Resolve-AudioPlayerBuildDir -BuildDir ""
$buildRoot = Resolve-AudioPlayerRepoPath -RepoRoot $repoRoot -Path $buildDir
if ([string]::IsNullOrWhiteSpace($OutputScript)) {
    $scriptSuffix = if ($Toolchain -eq "msvc") { "-msvc" } else { "" }
    $OutputScript = Join-Path $buildRoot "ffmpeg-audio-core-$Profile$scriptSuffix.sh"
} elseif (-not [System.IO.Path]::IsPathRooted($OutputScript)) {
    $OutputScript = Join-Path $repoRoot $OutputScript
}

$commonFlags = @(
    "--disable-autodetect",
    "--disable-avdevice",
    "--disable-debug",
    "--disable-doc",
    "--disable-everything",
    "--disable-network",
    "--disable-stripping",
    "--disable-shared",
    "--enable-avcodec",
    "--enable-avfilter",
    "--enable-avformat",
    "--enable-ffmpeg",
    "--enable-swresample"
)

$profileFlags = if ($Profile -eq "runtime-with-ffprobe") {
    @("--enable-ffprobe")
} else {
    @("--disable-ffprobe")
}

$modules = [ordered]@{
    Protocols = @(
        "file",
        "pipe"
    )
    Demuxers = @(
        "aac",
        "ac3",
        "eac3",
        "flac",
        "matroska",
        "mov",
        "mp3",
        "truehd",
        "wav"
    )
    Muxers = @(
        "matroska",
        "null",
        "pcm_f32le",
        "pcm_s16le",
        "pcm_s32le",
        "pcm_u8"
    )
    Decoders = @(
        "aac",
        "ac3",
        "alac",
        "eac3",
        "flac",
        "mp3",
        "mp3float",
        "pcm_f32le",
        "pcm_s16le",
        "pcm_s24le",
        "pcm_s32le",
        "pcm_u8",
        "truehd"
    )
    Encoders = @(
        "pcm_f32le",
        "pcm_s16le",
        "pcm_s32le",
        "pcm_u8"
    )
    Filters = @(
        "aformat",
        "aresample",
        "channelmap",
        "pan"
    )
    Parsers = @(
        "aac",
        "ac3",
        "flac",
        "mlp",
        "mpegaudio"
    )
}

$configureFlags = @()
$configureFlags += $commonFlags
$configureFlags += $profileFlags
$configureFlags += Join-ConfigureList -Prefix "--enable-protocol" -Values $modules.Protocols
$configureFlags += Join-ConfigureList -Prefix "--enable-demuxer" -Values $modules.Demuxers
$configureFlags += Join-ConfigureList -Prefix "--enable-muxer" -Values $modules.Muxers
$configureFlags += Join-ConfigureList -Prefix "--enable-decoder" -Values $modules.Decoders
$configureFlags += Join-ConfigureList -Prefix "--enable-encoder" -Values $modules.Encoders
$configureFlags += Join-ConfigureList -Prefix "--enable-filter" -Values $modules.Filters
$configureFlags += Join-ConfigureList -Prefix "--enable-parser" -Values $modules.Parsers

if ($Toolchain -eq "msvc") {
    $configureFlags += @(
        "--toolchain=msvc",
        "--arch=x86_64"
    )

    if ($DisableX86Asm) {
        $configureFlags += "--disable-x86asm"
    }
}

$scriptLines = @(
    "#!/usr/bin/env bash",
    "set -euo pipefail",
    "",
    "SOURCE_DIR=""`${SOURCE_DIR:-$SourceDirPlaceholder}""",
    "PREFIX_DIR=""`${PREFIX_DIR:-$PrefixDirPlaceholder}""",
    "JOBS=""`${JOBS:-4}""",
    "MAKE_CMD=""`${MAKE_CMD:-make}""",
    "",
    'if [[ ! -f "$SOURCE_DIR/configure" ]]; then',
    '  echo "FFmpeg source tree not found: $SOURCE_DIR" >&2',
    '  exit 1',
    'fi',
    "",
    'mkdir -p "$PREFIX_DIR"',
    'cd "$SOURCE_DIR"',
    'make distclean >/dev/null 2>&1 || true',
    "",
    'configure_args=(',
    '  "--prefix=$PREFIX_DIR"'
)

foreach ($flag in $configureFlags) {
    $scriptLines += "  `"$flag`""
}

$scriptLines += @(
    ')',
    '',
    'configure_cmd=(./configure)',
    '[[ -n "${FFMPEG_CC:-}" ]] && configure_cmd+=("--cc=$FFMPEG_CC")',
    '[[ -n "${FFMPEG_LD:-}" ]] && configure_cmd+=("--ld=$FFMPEG_LD")',
    '[[ -n "${FFMPEG_AR:-}" ]] && configure_cmd+=("--ar=$FFMPEG_AR")',
    '[[ -n "${FFMPEG_NM:-}" ]] && configure_cmd+=("--nm=$FFMPEG_NM")',
    '"${configure_cmd[@]}" "${configure_args[@]}"',
    '"$MAKE_CMD" -j"$JOBS"',
    '"$MAKE_CMD" install'
)

$outputDir = Split-Path -Parent $OutputScript
if (-not [string]::IsNullOrWhiteSpace($outputDir)) {
    $null = New-Item -ItemType Directory -Force -Path $outputDir
}

Set-Content -Path $OutputScript -Value $scriptLines -Encoding ascii
Write-Output "profile:$Profile"
Write-Output "toolchain:$Toolchain"
Write-Output "script:$OutputScript"
Write-Output "sourcePlaceholder:$SourceDirPlaceholder"
Write-Output "prefixPlaceholder:$PrefixDirPlaceholder"

foreach ($moduleKey in $modules.Keys) {
    Write-Output ("{0}:{1}" -f $moduleKey, ($modules[$moduleKey] -join ','))
}

if ($PrintScript) {
    Write-Output "-----"
    Get-Content -Path $OutputScript
}

if ($RunBuild) {
    if ($Toolchain -ne "msvc") {
        throw "RunBuild currently supports only -Toolchain msvc."
    }

    if ([string]::IsNullOrWhiteSpace($SourceDir)) {
        $SourceDir = [System.Environment]::GetEnvironmentVariable("AUDIOPLAYER_FFMPEG_SOURCE_DIR")
    }
    if ([string]::IsNullOrWhiteSpace($SourceDir)) {
        $SourceDir = Join-Path $buildRoot "ffmpeg-src"
    } elseif (-not [System.IO.Path]::IsPathRooted($SourceDir)) {
        $SourceDir = Join-Path $repoRoot $SourceDir
    }

    if (-not (Test-Path $SourceDir)) {
        Write-Output "FFmpeg source not found at $SourceDir, cloning FFmpeg $script:FfmpegVersion..."
        git clone --branch "n$script:FfmpegVersion" --depth 1 https://git.ffmpeg.org/ffmpeg.git $SourceDir
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to clone FFmpeg source from https://git.ffmpeg.org/ffmpeg.git"
        }
    }

    $sourceReleasePath = Join-Path $SourceDir "RELEASE"
    $sourceRelease = if (Test-Path $sourceReleasePath -PathType Leaf) {
        (Get-Content -Path $sourceReleasePath -Raw).Trim()
    } else {
        "unknown"
    }
    if ($sourceRelease -ne $script:FfmpegVersion) {
        throw "FFmpeg source version mismatch: expected $script:FfmpegVersion, found $sourceRelease under $SourceDir. Point -SourceDir at FFmpeg $script:FfmpegVersion or remove the stale source tree."
    }

    if ([string]::IsNullOrWhiteSpace($PrefixDir)) {
        $PrefixDir = [System.Environment]::GetEnvironmentVariable("AUDIOPLAYER_FFMPEG_AUDIO_CORE_PREFIX")
    }
    if ([string]::IsNullOrWhiteSpace($PrefixDir)) {
        $PrefixDir = Join-Path $buildRoot "ffmpeg-audio-core\$Profile-msvc"
    } elseif (-not [System.IO.Path]::IsPathRooted($PrefixDir)) {
        $PrefixDir = Join-Path $repoRoot $PrefixDir
    }

    if ([string]::IsNullOrWhiteSpace($VcVars64Path)) {
        $VcVars64Path = Resolve-VcVars64Path
    }

    if ([string]::IsNullOrWhiteSpace($Msys2ShellPath)) {
        $Msys2ShellPath = Resolve-Msys2ShellPath
    }

    if ([string]::IsNullOrWhiteSpace($VcVars64Path)) {
        throw "Unable to locate vcvars64.bat. Set -VcVars64Path or AUDIOPLAYER_VCVARS64_PATH."
    }

    if ([string]::IsNullOrWhiteSpace($Msys2ShellPath)) {
        throw "Unable to locate msys2_shell.cmd. Set -Msys2ShellPath, AUDIOPLAYER_MSYS2_SHELL_PATH, MSYS2_ROOT, or put msys2_shell.cmd on PATH."
    }

    foreach ($requiredPath in @($SourceDir, $VcVars64Path, $Msys2ShellPath)) {
        if (-not (Test-Path $requiredPath)) {
            throw "Required path not found: $requiredPath"
        }
    }

    $envMap = Get-EnvironmentFromBatchFile -BatchFile $VcVars64Path
    foreach ($pair in $envMap.GetEnumerator()) {
        [Environment]::SetEnvironmentVariable($pair.Key, $pair.Value)
    }

    $clCandidates = @(where.exe cl.exe 2>$null | Where-Object { $_ -match 'Microsoft Visual Studio' })
    if ($clCandidates.Count -eq 0) {
        throw "Unable to locate cl.exe after loading vcvars64."
    }

    $vcBinDir = Split-Path -Parent $clCandidates[0]
    $ccPath = Join-Path $vcBinDir "cl.exe"
    $ldPath = Join-Path $vcBinDir "link.exe"
    $arPath = Join-Path $vcBinDir "lib.exe"
    $nmPath = Join-Path $vcBinDir "dumpbin.exe"

    $toolWrapperDir = Join-Path $buildRoot "msys2-msvc-tools"
    Write-Msys2MsvcToolWrappers -WrapperDirectory $toolWrapperDir `
                                -CcPath $ccPath `
                                -LdPath $ldPath `
                                -ArPath $arPath `
                                -NmPath $nmPath

    $sourceBashPath = Convert-ToBashPath -Path $SourceDir
    $prefixBashPath = Convert-ToBashPath -Path $PrefixDir
    $scriptBashPath = Convert-ToBashPath -Path $OutputScript
    $ccWrapperBashPath = Convert-ToBashPath -Path (Join-Path $toolWrapperDir "cl.exe")
    $ldWrapperBashPath = Convert-ToBashPath -Path (Join-Path $toolWrapperDir "link.exe")
    $arWrapperBashPath = Convert-ToBashPath -Path (Join-Path $toolWrapperDir "lib.exe")
    $nmWrapperBashPath = Convert-ToBashPath -Path (Join-Path $toolWrapperDir "dumpbin.exe")
    $currentPath = [Environment]::GetEnvironmentVariable("PATH", "Process")
    $pathSegments = @($toolWrapperDir, $currentPath) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }

    [Environment]::SetEnvironmentVariable("PATH", ($pathSegments -join ';'), "Process")
    [Environment]::SetEnvironmentVariable("MSYS2_PATH_TYPE", "inherit", "Process")
    [Environment]::SetEnvironmentVariable("SOURCE_DIR", $sourceBashPath, "Process")
    [Environment]::SetEnvironmentVariable("PREFIX_DIR", $prefixBashPath, "Process")
    [Environment]::SetEnvironmentVariable("JOBS", $Jobs.ToString([System.Globalization.CultureInfo]::InvariantCulture), "Process")
    [Environment]::SetEnvironmentVariable("MAKE_CMD", "make", "Process")
    [Environment]::SetEnvironmentVariable("FFMPEG_CC", $ccWrapperBashPath, "Process")
    [Environment]::SetEnvironmentVariable("FFMPEG_LD", $ldWrapperBashPath, "Process")
    [Environment]::SetEnvironmentVariable("FFMPEG_AR", $arWrapperBashPath, "Process")
    [Environment]::SetEnvironmentVariable("FFMPEG_NM", $nmWrapperBashPath, "Process")

    $requiredMsys2Commands = @("make", "nasm", "pkgconf")
    foreach ($commandName in $requiredMsys2Commands) {
        Invoke-Msys2Command -Msys2ShellPath $Msys2ShellPath -Command "command -v $commandName >/dev/null"
    }

    $stampPath = Join-Path $PrefixDir "build-profile.stamp"
    $sourceConfigureSha256 = Get-FileSha256 -Path (Join-Path $SourceDir "configure")
    $expectedStamp = (@(
            "profile=$Profile",
            "toolchain=$Toolchain",
            "disableX86Asm=$($DisableX86Asm.IsPresent)",
            "scriptSha256=$(Get-FileSha256 -Path $OutputScript)",
            "sourceRelease=$sourceRelease",
            "sourceConfigureSha256=$sourceConfigureSha256"
        ) -join "`n")
    $primaryBinary = Join-Path $PrefixDir "bin\ffmpeg.exe"
    $probeBinary = Join-Path $PrefixDir "bin\ffprobe.exe"
    $probeRequired = $Profile -eq "runtime-with-ffprobe"
    $binarySetReady = (Test-Path $primaryBinary -PathType Leaf) -and (-not $probeRequired -or (Test-Path $probeBinary -PathType Leaf))
    if (-not $ForceRebuild -and $binarySetReady -and (Test-Path $stampPath -PathType Leaf)) {
        $currentStamp = Get-Content -Path $stampPath -Raw
        if ((Normalize-TextForComparison -Text $currentStamp) -eq (Normalize-TextForComparison -Text $expectedStamp)) {
            Write-Output "buildSkipped:$PrefixDir"
            Write-Output "runtimeVersion:$(Get-ExecutableVersionLine -Path $primaryBinary)"
            return
        }
    }

    # MSYS2 bash splits unquoted paths at spaces. If any key path contains
    # spaces, create a temporary SUBST drive so FFmpeg's configure sees a
    # space-free path.
    $substDrive = ""
    $pathsNeedingMapping = @($SourceDir, $PrefixDir, $buildRoot, (Split-Path -Parent $OutputScript))
    $hasSpaces = $pathsNeedingMapping | Where-Object { $_ -match ' ' }
    if ($hasSpaces) {
        foreach ($letter in @("Q","R","S","T","U","V","W")) {
            $candidate = "${letter}:"
            if (-not (Test-Path $candidate)) {
                try {
                    $null = & subst $candidate $buildRoot
                    if ($LASTEXITCODE -eq 0) {
                        $substDrive = $candidate
                        break
                    }
                } catch { }
            }
        }
        if ([string]::IsNullOrWhiteSpace($substDrive)) {
            throw "Cannot create SUBST drive for path with spaces. Free up a drive letter (Q:-W:)."
        }
        $buildRootSlash = Convert-ToBashPath -Path $buildRoot
        $substSlash = "/" + $substDrive.Substring(0, 1).ToLowerInvariant()
        $sourceBashPath = $sourceBashPath -replace [regex]::Escape($buildRootSlash), $substSlash
        $prefixBashPath = $prefixBashPath -replace [regex]::Escape($buildRootSlash), $substSlash
        $scriptBashPath = $scriptBashPath -replace [regex]::Escape($buildRootSlash), $substSlash
        $ccWrapperBashPath = $ccWrapperBashPath -replace [regex]::Escape($buildRootSlash), $substSlash
        $ldWrapperBashPath = $ldWrapperBashPath -replace [regex]::Escape($buildRootSlash), $substSlash
        $arWrapperBashPath = $arWrapperBashPath -replace [regex]::Escape($buildRootSlash), $substSlash
        $nmWrapperBashPath = $nmWrapperBashPath -replace [regex]::Escape($buildRootSlash), $substSlash
        [Environment]::SetEnvironmentVariable("SOURCE_DIR", $sourceBashPath, "Process")
        [Environment]::SetEnvironmentVariable("PREFIX_DIR", $prefixBashPath, "Process")
        [Environment]::SetEnvironmentVariable("FFMPEG_CC", $ccWrapperBashPath, "Process")
        [Environment]::SetEnvironmentVariable("FFMPEG_LD", $ldWrapperBashPath, "Process")
        [Environment]::SetEnvironmentVariable("FFMPEG_AR", $arWrapperBashPath, "Process")
        [Environment]::SetEnvironmentVariable("FFMPEG_NM", $nmWrapperBashPath, "Process")
        Write-Output "substDrive:$substDrive (mapped $buildRoot)"
    }

    try {
        & $Msys2ShellPath -msys -defterm -no-start -use-full-path -here -shell bash $scriptBashPath
        if ($LASTEXITCODE -ne 0) {
            throw "FFmpeg build failed with exit code $LASTEXITCODE"
        }
    } finally {
        if (-not [string]::IsNullOrWhiteSpace($substDrive)) {
            & subst $substDrive /d 2>$null
        }
    }

    $null = New-Item -ItemType Directory -Force -Path $PrefixDir
    [System.IO.File]::WriteAllText($stampPath, $expectedStamp, [System.Text.Encoding]::ASCII)
    Write-Output "builtPrefix:$PrefixDir"
    Write-Output "runtimeVersion:$(Get-ExecutableVersionLine -Path $primaryBinary)"
}
