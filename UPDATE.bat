@echo off
setlocal EnableExtensions EnableDelayedExpansion

rem ============================================================================
rem  Red Dead Redemption VR - Update and Install
rem
rem  Gets Git if this PC has none (a private portable copy, no admin rights),
rem  brings this folder to the latest version of the mod from GitHub and then
rem  starts BUILD_AND_INSTALL.bat.
rem
rem  Works in a folder made by "git clone" and in a folder unpacked from a
rem  GitHub ZIP: the first run turns the ZIP folder into a Git copy. Files the
rem  mod does not track (your exported APKs, build output, the .tools folder)
rem  are never touched. Changes you made to the mod's own files are replaced
rem  by the official version.
rem ============================================================================

rem Git rewrites this file while it runs, which confuses cmd.exe. Run a copy
rem from the temp folder instead and pass the mod folder along.
if not defined RDRVR_UPDATE_DIR (
  set "RDRVR_UPDATE_DIR=%~dp0"
  set "RDRVR_UPDATE_COPY=%TEMP%\rdrvr-update-%RANDOM%%RANDOM%.bat"
  copy /y "%~f0" "!RDRVR_UPDATE_COPY!" >nul
  if errorlevel 1 (
    echo ERROR: could not copy the updater to "%TEMP%".
    pause
    exit /b 1
  )
  "%ComSpec%" /d /c call "!RDRVR_UPDATE_COPY!" %*
  set "RDRVR_UPDATE_EXIT=!errorlevel!"
  del "!RDRVR_UPDATE_COPY!" >nul 2>&1
  exit /b !RDRVR_UPDATE_EXIT!
)

title Red Dead Redemption VR - Update and Install
cd /d "%RDRVR_UPDATE_DIR%"

rem Official repository and branch. A Git copy that already has an "origin"
rem keeps its own address.
set "REPO_URL=https://github.com/dubrovskiy-yevhen-stakelogic/rdr1-quest.git"
set "BRANCH=main"
if defined RDRVR_REPO_URL set "REPO_URL=%RDRVR_REPO_URL%"
set "TOOLS_DIR=%RDRVR_UPDATE_DIR%.tools"
set "GIT_VER=2.47.1"
set "GIT_URL=https://github.com/git-for-windows/git/releases/download/v%GIT_VER%.windows.1/PortableGit-%GIT_VER%-64-bit.7z.exe"
set "GIT_EXE=git"

echo.
echo ============================================================
echo   RED DEAD REDEMPTION VR - UPDATE AND INSTALL
echo ============================================================
echo.

rem --- 1. Git -----------------------------------------------------------------
where git >nul 2>&1
if not errorlevel 1 (
  echo [1/3] Git found on this PC.
  goto :have_git
)
if exist "%TOOLS_DIR%\PortableGit\cmd\git.exe" (
  echo [1/3] Using the private Git copy from a previous run.
  set "GIT_EXE=%TOOLS_DIR%\PortableGit\cmd\git.exe"
  goto :have_git
)
echo [1/3] Git is not installed. Downloading a private copy, no admin needed...
if not exist "%TOOLS_DIR%" mkdir "%TOOLS_DIR%"
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; $ProgressPreference='SilentlyContinue'; try { Invoke-WebRequest -UseBasicParsing -Uri '%GIT_URL%' -OutFile '%TOOLS_DIR%\PortableGit.7z.exe' } catch { Write-Host $_.Exception.Message; exit 1 }"
if not exist "%TOOLS_DIR%\PortableGit.7z.exe" (
  echo.
  echo ERROR: could not download Git. Check the internet connection and run
  echo this file again, or install Git from https://git-scm.com
  goto :fail
)
echo       Extracting Git...
"%TOOLS_DIR%\PortableGit.7z.exe" -y -o"%TOOLS_DIR%\PortableGit" >nul
del "%TOOLS_DIR%\PortableGit.7z.exe" >nul 2>&1
if not exist "%TOOLS_DIR%\PortableGit\cmd\git.exe" (
  echo ERROR: Git could not be extracted.
  goto :fail
)
set "GIT_EXE=%TOOLS_DIR%\PortableGit\cmd\git.exe"

:have_git
rem The private Git copy and the updater state stay out of the mod's files.
if exist ".git\info" (
  findstr /x /c:"/.tools/" ".git\info\exclude" >nul 2>&1 || (echo /.tools/>>".git\info\exclude")
)

rem --- 2. Latest version ------------------------------------------------------
if not exist ".git" (
  echo [2/3] First run in this folder: connecting it to the official repository...
  "%GIT_EXE%" init -q
  if errorlevel 1 goto :gitfail
  "%GIT_EXE%" remote add origin "%REPO_URL%"
  if errorlevel 1 goto :gitfail
  echo /.tools/>>".git\info\exclude"
) else (
  "%GIT_EXE%" remote get-url origin >nul 2>&1
  if errorlevel 1 (
    "%GIT_EXE%" remote add origin "%REPO_URL%"
    if errorlevel 1 goto :gitfail
  )
)
for /f "delims=" %%U in ('"%GIT_EXE%" remote get-url origin') do set "ORIGIN_URL=%%U"
echo [2/3] Downloading the latest version from !ORIGIN_URL! ...
set "OLD_HEAD="
for /f "delims=" %%H in ('"%GIT_EXE%" rev-parse --verify -q HEAD 2^>nul') do set "OLD_HEAD=%%H"
"%GIT_EXE%" -c core.autocrlf=false fetch --depth 1 origin "%BRANCH%"
if errorlevel 1 goto :gitfail
"%GIT_EXE%" -c core.autocrlf=false reset -q --hard FETCH_HEAD
if errorlevel 1 goto :gitfail
"%GIT_EXE%" branch -q -f "%BRANCH%" FETCH_HEAD >nul 2>&1
"%GIT_EXE%" checkout -q "%BRANCH%" >nul 2>&1
set "NEW_HEAD="
for /f "delims=" %%H in ('"%GIT_EXE%" rev-parse --short HEAD') do set "NEW_HEAD=%%H"
if /i "!OLD_HEAD:~0,7!"=="!NEW_HEAD!" (
  echo       Already up to date ^(!NEW_HEAD!^).
) else (
  echo       Updated to !NEW_HEAD!.
)

rem --- 3. Build and install ---------------------------------------------------
if defined RDRVR_UPDATE_NO_BUILD (
  echo [3/3] Skipped: RDRVR_UPDATE_NO_BUILD is set.
  exit /b 0
)
if not exist "BUILD_AND_INSTALL.bat" (
  echo ERROR: BUILD_AND_INSTALL.bat is missing from the downloaded version.
  goto :fail
)
echo [3/3] Starting the build and install wizard...
echo.
set "RDRVR_KEEP_OPEN=1"
call "BUILD_AND_INSTALL.bat" %*
exit /b %errorlevel%

:gitfail
echo.
echo ERROR: the mod could not be downloaded. Check the internet connection
echo and that GitHub is reachable, then run this file again.
goto :fail

:fail
echo.
pause
exit /b 1
