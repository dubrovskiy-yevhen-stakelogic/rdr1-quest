@echo off
setlocal EnableExtensions

rem Keep a double-clicked window open so success and errors remain visible.
if not defined RDRVR_EXPORT_KEEP_OPEN (
  set "RDRVR_EXPORT_KEEP_OPEN=1"
  "%ComSpec%" /d /k call "%~f0" %*
  exit /b
)

title Red Dead Redemption VR - Export Game From Android Phone
cd /d "%~dp0"

echo ================================================================
echo  RED DEAD REDEMPTION - EXPORT YOUR GAME FROM AN ANDROID PHONE
echo ================================================================
echo.
echo Connect the Android phone or tablet where Red Dead Redemption
echo (Google Play version) is installed and has been launched
echo once, then approve USB debugging on the phone.
echo Seeing the phone in Windows File Explorer is not enough by itself.
echo.
echo About 6 GB of free disk space is needed on drive C:.
echo.

set "RDRVR_EXPORT=%~dp0tools\export-rdr-apks.ps1"
if not exist "%RDRVR_EXPORT%" (
  echo ERROR: The source kit is incomplete. Missing:
  echo   %RDRVR_EXPORT%
  set "RESULT=1"
  goto :finished
)

powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%RDRVR_EXPORT%" %*
set "RESULT=%ERRORLEVEL%"

:finished
echo.
if "%RESULT%"=="0" (
  echo EXPORT COMPLETED.
) else (
  echo EXPORT FAILED. Read the error above.
)
echo.
echo This window will remain open. Press any key when you are done reading it.
pause >nul
exit /b %RESULT%
