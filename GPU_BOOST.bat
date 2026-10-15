@echo off
setlocal EnableExtensions

rem Keep a double-clicked window open so success and errors remain visible.
if not defined RDRVR_BOOST_KEEP_OPEN (
  set "RDRVR_BOOST_KEEP_OPEN=1"
  "%ComSpec%" /d /k call "%~f0" %*
  exit /b
)

title Red Dead Redemption VR - GPU Boost
echo.
echo ============================================================
echo   RED DEAD REDEMPTION VR - GPU BOOST HELPER
echo ============================================================
echo.
echo This starts a small helper on the headset that raises the GPU clock
echo only while the game runs and asks for it. Home and other apps keep
echo their normal clocks. Run it once after every headset restart.
echo The GPU BOOST row of the GRAPHICS menu chooses OFF, 640 or 690 MHz
echo and shows NO HELPER when the helper is not running.
echo Add -Off to stop the helper, or -Status to show its state.
echo.

set "RDRVR_BOOST=%~dp0tools\gpu-boost.ps1"
if not exist "%RDRVR_BOOST%" (
  echo ERROR: The source kit is incomplete. Missing:
  echo   %RDRVR_BOOST%
  set "RDRVR_EXIT=1"
  goto :finished
)

where powershell.exe >nul 2>&1
if errorlevel 1 (
  echo ERROR: Windows PowerShell was not found.
  set "RDRVR_EXIT=1"
  goto :finished
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%RDRVR_BOOST%" %*
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
