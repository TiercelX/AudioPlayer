@echo off
setlocal EnableExtensions EnableDelayedExpansion

cd /d "%~dp0"
set "CONFIGURATION=%~1"
if not defined CONFIGURATION set "CONFIGURATION=Debug"
if /I not "%CONFIGURATION%"=="Debug" if /I not "%CONFIGURATION%"=="Release" if /I not "%CONFIGURATION%"=="RelWithDebInfo" (
    echo Unsupported configuration: %CONFIGURATION%
    echo Expected Debug, Release, or RelWithDebInfo.
    exit /b 1
)
echo Building 0523 ASIO %CONFIGURATION% target...
echo.

set "BUILD_EXIT=0"
set "BUILD_DIR=%~dp0build-mimo-asio"
set "LOG_DIR=%~dp0build-claude-logs"
set "TARGET_RUNTIME=%BUILD_DIR%\ffmpeg-audio-core\runtime-with-ffprobe-msvc"
set "FFMPEG_SOURCE_DIR=%BUILD_DIR%\ffmpeg-src"
set "LATEST_FILE=%BUILD_DIR%\playable\%CONFIGURATION%\LATEST.txt"

if not exist "%LOG_DIR%" mkdir "%LOG_DIR%" >nul 2>nul
set "LOG_STAMP="
for /f "usebackq tokens=*" %%I in (`powershell.exe -NoProfile -Command "[guid]::NewGuid().ToString('N')" 2^>nul`) do set "LOG_STAMP=%%I"
if not defined LOG_STAMP set "LOG_STAMP=fallback-%RANDOM%-%RANDOM%-%RANDOM%-%RANDOM%"
set "FFMPEG_LOG_FILE=%LOG_DIR%\build-ffmpeg-audio-core-%LOG_STAMP%.log"

if exist "%~dp0build-mimo-asio\AudioPlayer.exe" (
    del /q "%~dp0build-mimo-asio\AudioPlayer.exe" >nul 2>nul
)

echo Step 1/2: preparing self-built ffmpeg audio-core runtime...
echo ffmpegBuildLog:%FFMPEG_LOG_FILE%
echo [build-mimo-asio] repo=%~dp0 > "%FFMPEG_LOG_FILE%"
echo [build-mimo-asio] targetRuntime=%TARGET_RUNTIME% >> "%FFMPEG_LOG_FILE%"
echo [build-mimo-asio] sourceDir=%FFMPEG_SOURCE_DIR% >> "%FFMPEG_LOG_FILE%"

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%" >nul 2>nul
if not exist "%FFMPEG_SOURCE_DIR%" (
    echo First-time FFmpeg source clone may take a moment.
    echo [build-mimo-asio] cloning FFmpeg n7.1 >> "%FFMPEG_LOG_FILE%"
    git clone --depth 1 --branch n7.1 https://github.com/FFmpeg/FFmpeg.git "%FFMPEG_SOURCE_DIR%" >> "%FFMPEG_LOG_FILE%" 2>&1
    if errorlevel 1 (
        set "BUILD_EXIT=%ERRORLEVEL%"
        echo Failed to clone FFmpeg source into "%FFMPEG_SOURCE_DIR%".
        echo See log: "%FFMPEG_LOG_FILE%"
        goto finish
    )
)

if not exist "%FFMPEG_SOURCE_DIR%\configure" (
    set "BUILD_EXIT=1"
    echo FFmpeg source directory exists but is incomplete: "%FFMPEG_SOURCE_DIR%"
    echo Expected "%FFMPEG_SOURCE_DIR%\configure".
    echo Move or delete that directory, then run this script again.
    echo See log: "%FFMPEG_LOG_FILE%"
    goto finish
)

echo Building or checking FFmpeg audio-core. This can take several minutes after profile changes.
set "AUDIOPLAYER_BUILD_DIR=build-mimo-asio"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\build-ffmpeg-audio-core.ps1" -Toolchain msvc -RunBuild >> "%FFMPEG_LOG_FILE%" 2>&1
set "BUILD_EXIT=%ERRORLEVEL%"
if not "%BUILD_EXIT%"=="0" (
    echo FFmpeg audio-core build failed with exit code %BUILD_EXIT%.
    echo See log: "%FFMPEG_LOG_FILE%"
    goto finish
)
findstr /B /C:"buildSkipped:" /C:"builtPrefix:" "%FFMPEG_LOG_FILE%"
echo.

echo Step 2/2: building AudioPlayer %CONFIGURATION% bundle...
call "%~dp0scripts\build-app-msvc.cmd" -BuildDir build-mimo-asio -Configuration %CONFIGURATION%
set "BUILD_EXIT=%ERRORLEVEL%"

:finish
echo.
if "%BUILD_EXIT%"=="0" (
    echo Build completed successfully.
    if exist "%LATEST_FILE%" (
        set "PLAYABLE_DIR="
        set /p PLAYABLE_DIR=<"%LATEST_FILE%"
        if defined PLAYABLE_DIR echo Playable bundle: !PLAYABLE_DIR!
    )
) else (
    echo Build failed with exit code %BUILD_EXIT%.
)
echo.
echo Press any key to close this window.
pause >nul
exit /b %BUILD_EXIT%
