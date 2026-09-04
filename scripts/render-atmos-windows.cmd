@echo off
setlocal
set "SCRIPT_DIR=%~dp0"
where pwsh.exe >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    pwsh.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%run-atmos-windows.ps1" -Mode Render %*
) else (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%run-atmos-windows.ps1" -Mode Render %*
)
set "EXIT_CODE=%ERRORLEVEL%"
endlocal & exit /b %EXIT_CODE%
