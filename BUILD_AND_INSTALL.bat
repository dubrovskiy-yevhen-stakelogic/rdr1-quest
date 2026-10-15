@echo off
setlocal EnableExtensions

rem A double-clicked CMD window normally disappears on an early PowerShell
rem failure. Re-enter through cmd /k once so the result always stays visible.
if not defined RDRVR_KEEP_OPEN (
  set "RDRVR_KEEP_OPEN=1"
  "%ComSpec%" /d /k call "%~f0" %*
  exit /b
)

title Red Dead Redemption VR - Personal Quest Builder
echo.
echo ============================================================
echo   RED DEAD REDEMPTION VR - PERSONAL BUILD AND INSTALL WIZARD
echo ============================================================
echo.
echo No APK or game data is included in this source kit.
echo The game comes from your own phone: run EXPORT_RDR_APKS.bat first.
echo.

set "RDRVR_MASTER=%~dp0tools\build-and-install.ps1"
if not exist "%RDRVR_MASTER%" (
  echo ERROR: The source kit is incomplete. Missing:
  echo   %RDRVR_MASTER%
  set "RDRVR_EXIT=1"
  goto :finished
)

where powershell.exe >nul 2>&1
if errorlevel 1 (
  echo ERROR: Windows PowerShell was not found.
  set "RDRVR_EXIT=1"
  goto :finished
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%RDRVR_MASTER%" %*
set "RDRVR_EXIT=%ERRORLEVEL%"

:finished
echo.
if not "%RDRVR_EXIT%"=="0" (
  echo FAILED. Read the reported error and diagnostic log above.
) else (
  echo SUCCESS. Every requested build/install step completed.
)
echo.
echo This window will remain open. Press any key when you are done reading it.
pause >nul
exit /b %RDRVR_EXIT%
