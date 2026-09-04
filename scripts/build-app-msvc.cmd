@echo off
setlocal EnableExtensions

chcp 65001 >nul 2>nul

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..") do set "REPO_ROOT=%%~fI"
set "LOG_DIR=%REPO_ROOT%\build-claude-logs"
if not exist "%LOG_DIR%" mkdir "%LOG_DIR%" >nul 2>nul
set "LOG_STAMP="
for /f "usebackq tokens=*" %%I in (`powershell.exe -NoProfile -Command "[guid]::NewGuid().ToString('N')" 2^>nul`) do set "LOG_STAMP=%%I"
if not defined LOG_STAMP set "LOG_STAMP=fallback-%RANDOM%-%RANDOM%-%RANDOM%-%RANDOM%"
set "LOG_FILE=%LOG_DIR%\build-app-msvc-%LOG_STAMP%.log"

if "%~1"=="" (
    set "BUILD_ARGS=-BuildDir build-mm -Configuration Debug"
) else (
    set "BUILD_ARGS=%*"
)

echo buildLog:%LOG_FILE%
echo [build-app-msvc] repo=%REPO_ROOT% > "%LOG_FILE%"
echo [build-app-msvc] args=%BUILD_ARGS% >> "%LOG_FILE%"

set "VSDEVCMD="
if defined AUDIOPLAYER_VSDEVCMD_PATH (
    if exist "%AUDIOPLAYER_VSDEVCMD_PATH%" set "VSDEVCMD=%AUDIOPLAYER_VSDEVCMD_PATH%"
)

if not defined VSDEVCMD (
    if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
        for /f "usebackq tokens=*" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do (
            if exist "%%I\Common7\Tools\VsDevCmd.bat" set "VSDEVCMD=%%I\Common7\Tools\VsDevCmd.bat"
        )
    )
)

if not defined VSDEVCMD if exist "D:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" set "VSDEVCMD=D:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat"
if not defined VSDEVCMD if exist "D:\Program Files\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat" set "VSDEVCMD=D:\Program Files\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat"
if not defined VSDEVCMD if exist "%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" set "VSDEVCMD=%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat"
if not defined VSDEVCMD if exist "%ProgramFiles%\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat" set "VSDEVCMD=%ProgramFiles%\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat"
if not defined VSDEVCMD if exist "D:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" set "VSDEVCMD=D:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"
if not defined VSDEVCMD if exist "D:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" set "VSDEVCMD=D:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
if not defined VSDEVCMD if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" set "VSDEVCMD=%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"
if not defined VSDEVCMD if exist "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" set "VSDEVCMD=%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"

if not defined VSDEVCMD (
    echo [build-app-msvc] ERROR: Unable to locate VsDevCmd.bat. Set AUDIOPLAYER_VSDEVCMD_PATH. >> "%LOG_FILE%"
    type "%LOG_FILE%"
    exit /b 1
)

echo [build-app-msvc] vsDevCmd=%VSDEVCMD% >> "%LOG_FILE%"
call "%VSDEVCMD%" -arch=x64 -host_arch=x64
if errorlevel 1 (
    echo [build-app-msvc] ERROR: VsDevCmd.bat failed. >> "%LOG_FILE%"
    type "%LOG_FILE%"
    exit /b 1
)
echo [build-app-msvc] VsDevCmd.bat completed. >> "%LOG_FILE%"

if defined AUDIOPLAYER_QT_TOOLS_CMAKE_DIR (
    if exist "%AUDIOPLAYER_QT_TOOLS_CMAKE_DIR%\cmake.exe" set "PATH=%AUDIOPLAYER_QT_TOOLS_CMAKE_DIR%;%PATH%"
)
if exist "D:\Qt\Tools\CMake_64\bin\cmake.exe" set "PATH=D:\Qt\Tools\CMake_64\bin;%PATH%"
if exist "C:\Qt\Tools\CMake_64\bin\cmake.exe" set "PATH=C:\Qt\Tools\CMake_64\bin;%PATH%"
if exist "D:\Qt\Tools\Ninja\ninja.exe" set "PATH=D:\Qt\Tools\Ninja;%PATH%"
if exist "C:\Qt\Tools\Ninja\ninja.exe" set "PATH=C:\Qt\Tools\Ninja;%PATH%"

where cmake >> "%LOG_FILE%" 2>&1
if errorlevel 1 (
    echo [build-app-msvc] ERROR: cmake.exe not found after loading VS environment and Qt tools path. >> "%LOG_FILE%"
    type "%LOG_FILE%"
    exit /b 1
)

where nmake >> "%LOG_FILE%" 2>&1
if errorlevel 1 (
    echo [build-app-msvc] ERROR: nmake.exe not found after loading VS environment. >> "%LOG_FILE%"
    type "%LOG_FILE%"
    exit /b 1
)

cd /d "%REPO_ROOT%"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%build-app.ps1" %BUILD_ARGS% >> "%LOG_FILE%" 2>&1
set "EXIT_CODE=%ERRORLEVEL%"

type "%LOG_FILE%"
echo buildLog:%LOG_FILE%
exit /b %EXIT_CODE%
