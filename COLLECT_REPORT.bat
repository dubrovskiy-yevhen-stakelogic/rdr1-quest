@echo off
setlocal EnableExtensions

rem Keep a double-clicked window open so success and errors remain visible.
if not defined RDRVR_REPORT_KEEP_OPEN (
  set "RDRVR_REPORT_KEEP_OPEN=1"
  "%ComSpec%" /d /k call "%~f0" %*
  exit /b
)

title Red Dead Redemption VR - Collect Report
echo.
echo ============================================================
echo   RED DEAD REDEMPTION VR - COLLECT A HEADSET REPORT
echo ============================================================
echo.
echo This copies the mod report files, the VR settings and filtered
echo system logs from the headset into the reports folder of the work
echo directory and packs them into a ZIP file.
echo Nothing is uploaded and the game is not started.
echo.

set "RDRVR_REPORT=%~dp0tools\collect-report.ps1"
if not exist "%RDRVR_REPORT%" (
  echo ERROR: The source kit is incomplete. Missing:
  echo   %RDRVR_REPORT%
  set "RDRVR_EXIT=1"
  goto :finished
)

where powershell.exe >nul 2>&1
if errorlevel 1 (
  echo ERROR: Windows PowerShell was not found.
  set "RDRVR_EXIT=1"
  goto :finished
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%RDRVR_REPORT%" %*
set "RDRVR_EXIT=%ERRORLEVEL%"

:finished
echo.
if not "%RDRVR_EXIT%"=="0" (
  echo FAILED. Read the error above and check the USB connection.
) else (
  echo FINISHED. Review the report before sharing it.
)
echo.
echo This window will remain open. Press any key when you are done reading it.
pause >nul
exit /b %RDRVR_EXIT%
