@echo off
setlocal EnableExtensions

rem Keep a double-clicked window open so success and errors remain visible.
if not defined RDRVR_SAVES_KEEP_OPEN (
  set "RDRVR_SAVES_KEEP_OPEN=1"
  "%ComSpec%" /d /k call "%~f0" %*
  exit /b
)

title Red Dead Redemption VR - Transfer Saves
echo.
echo ============================================================
echo   RED DEAD REDEMPTION VR - MOVE SAVES BETWEEN PC AND HEADSET
echo ============================================================
echo.
echo This sends the saves of the PC game to the headset, from the newest
echo profile in Documents\Rockstar Games\Red Dead Redemption\Profiles.
echo They are imported the next time the game starts on the headset, and
echo every headset save they replace is backed up there first.
echo Drop a folder with .SAV files on this file to send that folder instead.
echo Add -FromHeadset to copy the headset saves to this PC, or -Status to
echo show the saves and the import log of the headset.
echo.

set "RDRVR_SAVES=%~dp0tools\transfer-saves.ps1"
if not exist "%RDRVR_SAVES%" (
  echo ERROR: The source kit is incomplete. Missing:
  echo   %RDRVR_SAVES%
  set "RDRVR_EXIT=1"
  goto :finished
)

where powershell.exe >nul 2>&1
if errorlevel 1 (
  echo ERROR: Windows PowerShell was not found.
  set "RDRVR_EXIT=1"
  goto :finished
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%RDRVR_SAVES%" %*
set "RDRVR_EXIT=%ERRORLEVEL%"

:finished
echo.
if not "%RDRVR_EXIT%"=="0" (
  echo FAILED. Read the error above and check the USB connection.
) else (
  echo FINISHED. The game was not launched.
)
echo.
echo This window will remain open. Press any key when you are done reading it.
pause >nul
exit /b %RDRVR_EXIT%
