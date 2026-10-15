@echo off
setlocal EnableExtensions

rem Keep a double-clicked window open so success and errors remain visible.
if not defined RDRVR_RESET_KEEP_OPEN (
  set "RDRVR_RESET_KEEP_OPEN=1"
  "%ComSpec%" /d /k call "%~f0" %*
  exit /b
)

title Red Dead Redemption VR - Reset VR Settings
echo.
echo ============================================================
echo   RED DEAD REDEMPTION VR - RESET HEADSET VR SETTINGS
echo ============================================================
echo.
echo This removes only the VR settings files of the mod. They are copied
echo to the settings-backups folder of the work directory first.
echo Saved games, saved teleport spots, reports, game data and the APK
echo are kept. Add -IncludeTeleportSpots to remove the teleport spots too.
echo.

set "RDRVR_RESET=%~dp0tools\reset-vr-settings.ps1"
if not exist "%RDRVR_RESET%" (
  echo ERROR: The source kit is incomplete. Missing:
  echo   %RDRVR_RESET%
  set "RDRVR_EXIT=1"
  goto :finished
)

where powershell.exe >nul 2>&1
if errorlevel 1 (
  echo ERROR: Windows PowerShell was not found.
  set "RDRVR_EXIT=1"
  goto :finished
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%RDRVR_RESET%" %*
set "RDRVR_EXIT=%ERRORLEVEL%"

:finished
echo.
if not "%RDRVR_EXIT%"=="0" (
  echo FAILED. No unreported files were removed.
) else (
  echo FINISHED. The game was not launched.
)
echo.
echo This window will remain open. Press any key when you are done reading it.
pause >nul
exit /b %RDRVR_EXIT%
