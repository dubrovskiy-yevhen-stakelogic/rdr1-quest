#!/usr/bin/env bash
# Linux/macOS personal builder and optional Quest installer.
# The game APKs are always supplied by the player.
set -euo pipefail
umask 077

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
MOD_VERSION="0.1.1"
USER_AGENT="RDRVR-SourceKit/0.1.1"
PACKAGE_NAME="com.rockstargames.rdr"
EXPECTED_VERSION_CODE="63226194"
EXPECTED_VERSION_NAME="1.58.63226194"
EXPECTED_ENGINE_SHA256="3F80845EDEC32A663C42A8570E3131446813E695076C4BC96E2142C190BC22F3"
THIRD_PARTY_SIGNER="358020B0ED9D910C3EB4E03E261EB340604833F8BDC75E236D965E80E63A56DE"
REMOTE_DATA_ROOT="/sdcard/Android/data/$PACKAGE_NAME"
REMOTE_FILES_ROOT="$REMOTE_DATA_ROOT/files"
REMOTE_OBB_ROOT="/sdcard/Android/obb/$PACKAGE_NAME"

GAME_PACKAGE=""
WORK_DIR="${HOME}/RDRVRBuild"
ANDROID_SDK=""
JAVA_HOME_ARG=""
SERIAL=""
LOG_PATH=""
KEYSTORE_ARG=""
TRADE="0"
BUILD_ONLY=0
NON_INTERACTIVE=0
PYTHON_EXE=""
BUILD_ROOT=""
PLAN_ROOT=""
RUN_ID=""
STAMP=""
KEY_LOCK=""
KEY_TEMP=""
KEYSTORE=""
BUILD_KEYSTORE=""
SIGNING_ROOT=""

JDK_VERSION="21.0.11+10"
ANDROID_TOOLS_VERSION="15859902"
PLATFORM_TOOLS_VERSION="37.0.1"
NDK_VERSION="27.2.12479018"
CMAKE_VERSION="3.22.1"
OPENXR_AAR_URL="https://repo1.maven.org/maven2/org/khronos/openxr/openxr_loader_for_android/1.1.43/openxr_loader_for_android-1.1.43.aar"
OPENXR_AAR_SHA256="7E1B36141F9A4F1FA4A7E061936344FD9FCD36BCE6C47EAE2AD09812736167B6"
OPENXR_LOADER_SHA256="713E3BB8D955254C670ACC1C4899A65CB8C930E97DD9958BF37EA922D72B7A06"
VULKAN_HEADERS_URL="https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.357.0.zip"
VULKAN_HEADERS_ARCHIVE="vulkan-sdk-1.4.357.0.zip"
VULKAN_HEADERS_SHA256="BC7925A17854A9BAC9433309E8DF88A701EF1D49D2020FFF6DB9407B637EA840"
VULKAN_HEADERS_ROOT_NAME="Vulkan-Headers-vulkan-sdk-1.4.357.0"

ADB=""
ADB_BASE=()
IS_MSYS=0
INSTALL_ACTIVE=0
REMOTE_STAGING=()
REMOTE_ROLLBACK_TARGETS=()
REMOTE_ROLLBACK_BACKUPS=()
REMOTE_ROLLBACK_HAD_EXISTING=()
SIGNER_HASH_DEC=""
APK_BYTES=0
EXTERNAL_BYTES=0
DEVICE_BACKUP=""

case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) IS_MSYS=1 ;; esac

usage() {
  cat <<'EOF'
Usage: tools/build-and-install.sh [options]

  --game-package PATH   Folder made by EXPORT_RDR_APKS.bat (base.apk and split_*.apk),
                        a single APK, or an .apks/.zip archive
  --work-dir DIR        Managed workspace (default: ~/RDRVRBuild)
  --android-sdk DIR     Android SDK root; bootstrapped when omitted
  --java-home DIR       JDK 21 root; bootstrapped when omitted
  --serial SERIAL       Quest serial when more than one device is connected
  --log-path FILE       Persistent diagnostic log
  --keystore FILE       Existing personal signing key to use
  --trade N             CPU/GPU trade level written to the manifest: -1, 0 or 1 (default 0)
  --build-only          Build and verify the APKs, but do not touch a Quest
  --non-interactive     Never prompt; missing inputs and approvals are fatal
  -h, --help            Show this help

Host prerequisites: Python 3.10 or newer (python3), curl, unzip and tar.
The game is force-stopped before and after installation and is never
launched by this tool.
EOF
}

die() {
  echo "ERROR: $*" >&2
  exit 1
}

absolute_existing() {
  local value="$1"
  local parent base
  parent="$(dirname "$value")"
  base="$(basename "$value")"
  printf '%s/%s\n' "$(cd "$parent" && pwd -P)" "$base"
}

normalize_prompt_path() {
  local value="$1" first last
  if [ "${#value}" -ge 2 ]; then
    first="${value:0:1}"
    last="${value: -1}"
    if { [ "$first" = '"' ] && [ "$last" = '"' ]; } ||
       { [ "$first" = "'" ] && [ "$last" = "'" ]; }; then
      value="${value:1:${#value}-2}"
    fi
  fi
  case "$value" in
    '~') value="$HOME" ;;
    '~/'*) value="$HOME/${value#\~/}" ;;
  esac
  value="${value//\\ / }"
  printf '%s\n' "$value"
}

canonicalize_directory_target() {
  local target="$1" current component resolved index
  local pending=()
  [ -n "$target" ] || die "directory path must not be empty"
  [[ "$target" != *$'\n'* && "$target" != *$'\r'* ]] ||
    die "directory path must stay on one line"
  case "$target" in
    /*) current="$target" ;;
    *) current="$PWD/$target" ;;
  esac
  while [ ! -e "$current" ]; do
    component="$(basename "$current")"
    [ -n "$component" ] && [ "$component" != "/" ] ||
      die "could not resolve directory target: $target"
    pending+=("$component")
    resolved="$(dirname "$current")"
    [ "$resolved" != "$current" ] || die "could not resolve directory target: $target"
    current="$resolved"
  done
  [ -d "$current" ] || die "directory target has a non-directory ancestor: $current"
  resolved="$(cd "$current" && pwd -P)"
  for ((index=${#pending[@]}-1; index>=0; index--)); do
    component="${pending[index]}"
    case "$component" in
      ''|.) ;;
      ..)
        if [ "$resolved" != "/" ]; then
          resolved="${resolved%/*}"
          [ -n "$resolved" ] || resolved="/"
        fi
        ;;
      *)
        if [ "$resolved" = "/" ]; then resolved="/$component"; else resolved="$resolved/$component"; fi
        ;;
    esac
  done
  printf '%s\n' "$resolved"
}

path_is_within() {
  local candidate="$1" parent="$2"
  case "$candidate/" in
    "$parent/"*) return 0 ;;
    *) return 1 ;;
  esac
}

remove_managed_tree() {
  local target="$1"
  local parent resolved_parent
  path_is_within "$target" "$WORK_DIR" && [ "$target" != "$WORK_DIR" ] ||
    die "refusing to remove a directory outside the managed work directory: $target"
  parent="$(dirname "$target")"
  [ -d "$parent" ] || die "managed parent directory is missing: $parent"
  resolved_parent="$(cd "$parent" && pwd -P)"
  path_is_within "$resolved_parent" "$WORK_DIR" ||
    die "managed parent resolves outside the work directory: $parent -> $resolved_parent"
  rm -rf -- "$target"
}

prepare_managed_directory() {
  local target="$1" resolved
  path_is_within "$target" "$WORK_DIR" && [ "$target" != "$WORK_DIR" ] ||
    die "refusing a managed directory outside the work directory: $target"
  [ ! -L "$target" ] || die "managed directory must not be a symlink: $target"
  if [ -e "$target" ] && [ ! -d "$target" ]; then
    die "managed path exists but is not a directory: $target"
  fi
  mkdir -p -- "$target"
  resolved="$(cd "$target" && pwd -P)"
  [ "$resolved" = "$target" ] ||
    die "managed directory resolves through a symlink: $target -> $resolved"
}

sha256_file() {
  local path="$1"
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$path" | awk '{print toupper($1)}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$path" | awk '{print toupper($1)}'
  else
    die "sha256sum or shasum is required"
  fi
}

download_verified() {
  local name="$1" url="$2" destination="$3" expected="$4"
  local partial observed aside
  [ ! -L "$destination" ] || die "$name cache path must not be a symlink: $destination"
  mkdir -p "$(dirname "$destination")"
  if [ -f "$destination" ]; then
    observed="$(sha256_file "$destination")"
    if [ "$observed" = "$expected" ]; then
      echo "Using verified cache: $destination" >&2
      printf '%s\n' "$destination"
      return 0
    fi
    aside="${destination}.invalid-$(date +%Y%m%d-%H%M%S)"
    mv "$destination" "$aside" || die "could not move the invalid cached $name aside"
    echo "WARNING: preserved an invalid cached file at: $aside" >&2
  fi
  partial="${destination}.partial"
  [ ! -L "$partial" ] || die "$name partial path must not be a symlink: $partial"
  echo "Downloading $name" >&2
  echo "  Source: $url" >&2
  curl -fL --retry 30 --retry-all-errors --retry-delay 3 -A "$USER_AGENT" -C - -o "$partial" "$url" >&2 ||
    die "$name download failed. Check the internet connection and run the builder again."
  observed="$(sha256_file "$partial")"
  if [ "$observed" != "$expected" ]; then
    rm -f -- "$partial"
    die "$name hash mismatch. Expected $expected, got $observed. Run the builder again to download it fresh."
  fi
  mv "$partial" "$destination"
  printf '%s\n' "$destination"
}

java_home_is_21() {
  local home="$1"
  [ -x "$home/bin/java" ] && [ -x "$home/bin/javac" ] || return 1
  "$home/bin/java" -version 2>&1 | head -n 1 | grep -Eq 'version "21([.]|")' &&
    "$home/bin/javac" -version 2>&1 | grep -Eq '^javac[[:space:]]+21([.]|$)'
}

configure_host_downloads() {
  local kernel arch
  kernel="$(uname -s)"
  arch="$(uname -m)"
  case "$kernel:$arch" in
    Linux:x86_64|Linux:amd64)
      JDK_ARCHIVE_NAME="OpenJDK21U-jdk_x64_linux_hotspot_21.0.11_10.tar.gz"
      JDK_SHA256="4B2220E232A97997B436CA6AB15CBF70171ECFF52958A46159DFA5A8C44CA4DE"
      ANDROID_ARCHIVE_NAME="commandlinetools-linux-${ANDROID_TOOLS_VERSION}_latest.zip"
      ANDROID_TOOLS_SHA256="4E4C464F145A7512B57D088AC6C278C03C9EEA610886B35A5E0804E74EEDF583"
      PLATFORM_TOOLS_ARCHIVE_NAME="platform-tools_r${PLATFORM_TOOLS_VERSION}-linux.zip"
      PLATFORM_TOOLS_SHA256="D230F13842F60F782A8645F9C813F8F845BF36089EA7289F28C48F17979313F1"
      HOST_TAG="linux-x64"
      ;;
    Darwin:x86_64)
      JDK_ARCHIVE_NAME="OpenJDK21U-jdk_x64_mac_hotspot_21.0.11_10.tar.gz"
      JDK_SHA256="34180EB03E6D207C388CCE3DA668F6CC7CD7508C185C24782FADAC2C9C0E66F9"
      ANDROID_ARCHIVE_NAME="commandlinetools-mac_x86_64-${ANDROID_TOOLS_VERSION}_latest.zip"
      ANDROID_TOOLS_SHA256="C5A6378AB5CF7E0D5701921405115BEFFF13E9FF7417FB588389338F8BD050F3"
      PLATFORM_TOOLS_ARCHIVE_NAME="platform-tools_r${PLATFORM_TOOLS_VERSION}-darwin.zip"
      PLATFORM_TOOLS_SHA256="EE39AD5967E95C2A07F04DBCBDE96B1A0C916BA376096DB5D2F498B7727A5D1D"
      HOST_TAG="mac-x64"
      ;;
    Darwin:arm64|Darwin:aarch64)
      JDK_ARCHIVE_NAME="OpenJDK21U-jdk_aarch64_mac_hotspot_21.0.11_10.tar.gz"
      JDK_SHA256="6EBCF221C9B41507B14C098E93C6EAD6440B8D9BD154F8EC666C4C73ABBDB201"
      ANDROID_ARCHIVE_NAME="commandlinetools-mac_arm64-${ANDROID_TOOLS_VERSION}_latest.zip"
      ANDROID_TOOLS_SHA256="835B62A26162B229B441D1F6D4680383815A270809EB33522C0D480FA5002C4E"
      PLATFORM_TOOLS_ARCHIVE_NAME="platform-tools_r${PLATFORM_TOOLS_VERSION}-darwin.zip"
      PLATFORM_TOOLS_SHA256="EE39AD5967E95C2A07F04DBCBDE96B1A0C916BA376096DB5D2F498B7727A5D1D"
      HOST_TAG="mac-arm64"
      ;;
    *) die "automatic tool bootstrap supports x86_64 Linux and x86_64/arm64 macOS; on Windows use BUILD_AND_INSTALL.bat" ;;
  esac
  JDK_URL="https://github.com/adoptium/temurin21-binaries/releases/download/jdk-21.0.11%2B10/${JDK_ARCHIVE_NAME}"
  ANDROID_TOOLS_URL="https://dl.google.com/android/repository/${ANDROID_ARCHIVE_NAME}"
  PLATFORM_TOOLS_URL="https://dl.google.com/android/repository/${PLATFORM_TOOLS_ARCHIVE_NAME}"
}

find_java_home() {
  local archive install_root
  if [ -n "$JAVA_HOME_ARG" ]; then
    java_home_is_21 "$JAVA_HOME_ARG" || die "--java-home must point to a complete JDK 21: $JAVA_HOME_ARG"
    (cd "$JAVA_HOME_ARG" && pwd -P)
    return 0
  fi

  configure_host_downloads
  install_root="$WORK_DIR/.tools/jdk-${JDK_VERSION}-${HOST_TAG}"
  if ! java_home_is_21 "$install_root" && ! java_home_is_21 "$install_root/Contents/Home"; then
    archive="$(download_verified "Eclipse Temurin JDK $JDK_VERSION" "$JDK_URL" \
      "$WORK_DIR/.downloads/$JDK_ARCHIVE_NAME" "$JDK_SHA256")"
    remove_managed_tree "$install_root"
    mkdir -p "$install_root"
    tar -xzf "$archive" -C "$install_root" --strip-components=1 || die "Temurin JDK extraction failed"
  fi
  if java_home_is_21 "$install_root"; then
    printf '%s\n' "$install_root"
  elif java_home_is_21 "$install_root/Contents/Home"; then
    printf '%s\n' "$install_root/Contents/Home"
  else
    die "Temurin JDK extraction did not produce a JDK 21 home"
  fi
}

ensure_android_command_line_tools() {
  local sdk_root="$1" candidate archive extract_root install_root invalid_root
  candidate="$sdk_root/cmdline-tools/$ANDROID_TOOLS_VERSION/bin/sdkmanager"
  if [ -f "$candidate" ]; then
    chmod +x "$candidate" 2>/dev/null || true
    printf '%s\n' "$candidate"
    return 0
  fi
  configure_host_downloads
  archive="$(download_verified "Android command-line tools $ANDROID_TOOLS_VERSION" \
    "$ANDROID_TOOLS_URL" "$WORK_DIR/.downloads/$ANDROID_ARCHIVE_NAME" "$ANDROID_TOOLS_SHA256")"
  extract_root="$WORK_DIR/.tools/android-command-line-tools-${ANDROID_TOOLS_VERSION}-${HOST_TAG}"
  install_root="$sdk_root/cmdline-tools/$ANDROID_TOOLS_VERSION"
  remove_managed_tree "$extract_root"
  mkdir -p "$extract_root" "$(dirname "$install_root")"
  unzip -q -o "$archive" -d "$extract_root" >&2 || die "Android command-line tools extraction failed"
  [ -f "$extract_root/cmdline-tools/bin/sdkmanager" ] || die "Android tools archive has an unexpected layout"
  if [ -e "$install_root" ]; then
    invalid_root="${install_root}.invalid-${STAMP}-$$"
    mv "$install_root" "$invalid_root" || die "could not preserve invalid Android CLI directory: $install_root"
    echo "Preserved invalid Android CLI directory: $invalid_root" >&2
  fi
  cp -R "$extract_root/cmdline-tools" "$install_root" || die "could not install the Android command-line tools"
  candidate="$install_root/bin/sdkmanager"
  [ -f "$candidate" ] || die "Android SDK bootstrap did not create sdkmanager"
  chmod +x "$candidate" 2>/dev/null || true
  printf '%s\n' "$candidate"
}

find_android_sdk() {
  local candidate
  if [ -n "$ANDROID_SDK" ]; then
    mkdir -p "$ANDROID_SDK"
    (cd "$ANDROID_SDK" && pwd -P)
    return 0
  fi
  candidate="$WORK_DIR/.android-sdk"
  mkdir -p "$candidate"
  (cd "$candidate" && pwd -P)
}

missing_sdk_packages() {
  local sdk="$1"
  [ -f "$sdk/platforms/android-35/android.jar" ] || echo "platforms;android-35"
  if [ ! -f "$sdk/build-tools/35.0.0/aapt2" ] ||
     [ ! -f "$sdk/build-tools/35.0.0/zipalign" ] ||
     [ ! -f "$sdk/build-tools/35.0.0/lib/apksigner.jar" ] ||
     [ ! -f "$sdk/build-tools/35.0.0/lib/d8.jar" ]; then
    echo "build-tools;35.0.0"
  fi
  [ -f "$sdk/ndk/$NDK_VERSION/build/cmake/android.toolchain.cmake" ] || echo "ndk;$NDK_VERSION"
  if [ ! -f "$sdk/cmake/$CMAKE_VERSION/bin/cmake" ] ||
     [ ! -f "$sdk/cmake/$CMAKE_VERSION/bin/ninja" ]; then
    echo "cmake;$CMAKE_VERSION"
  fi
}

licenses_accepted() {
  local sdkmanager="$1" sdk="$2" output
  [ -f "$sdk/licenses/android-sdk-license" ] || return 1
  output="$(set +o pipefail; yes n 2>/dev/null | "$sdkmanager" --sdk_root="$sdk" --licenses 2>&1)" || true
  printf '%s\n' "$output" | grep -q 'All SDK package licenses accepted'
}

ensure_sdk_packages() {
  local sdk="$1" sdkmanager answer rc line
  local missing=()
  while IFS= read -r line; do
    [ -n "$line" ] && missing+=("$line")
  done < <(missing_sdk_packages "$sdk")
  [ "${#missing[@]}" -gt 0 ] || return 0
  echo "Missing Android SDK components:"
  printf '  %s\n' "${missing[@]}"
  sdkmanager="$(ensure_android_command_line_tools "$sdk")"
  echo "Checking the recorded Android SDK license acceptance..."
  if ! licenses_accepted "$sdkmanager" "$sdk"; then
    [ "$NON_INTERACTIVE" -eq 0 ] ||
      die "Android SDK licenses have not been accepted in $sdk; run once without --non-interactive"
    [ -t 0 ] || die "Android SDK license acceptance needs an interactive terminal"
    echo ""
    echo "The Android SDK platform, build tools, NDK and CMake are installed with Google sdkmanager,"
    echo "which asks to accept the Android SDK License Agreement (https://developer.android.com/studio/terms)"
    echo "and the other SDK package licenses it lists. Type ACCEPT only if you agree to all of them;"
    echo "the builder then answers each sdkmanager license prompt with yes."
    read -r -p "Type ACCEPT to continue: " answer
    [ "$(printf '%s' "$answer" | tr '[:lower:]' '[:upper:]')" = "ACCEPT" ] ||
      die "Android SDK licenses were not accepted. No Android components were installed."
    set +e
    set +o pipefail
    yes | "$sdkmanager" --sdk_root="$sdk" --licenses
    rc="${PIPESTATUS[1]}"
    set -o pipefail
    set -e
    [ "$rc" -eq 0 ] && [ -f "$sdk/licenses/android-sdk-license" ] ||
      die "sdkmanager did not record the Android SDK license acceptance"
  fi
  echo "Installing Android SDK components. The NDK download is large; this takes a while."
  "$sdkmanager" --sdk_root="$sdk" "${missing[@]}" || die "Android SDK component installation failed"
  missing=()
  while IFS= read -r line; do
    [ -n "$line" ] && missing+=("$line")
  done < <(missing_sdk_packages "$sdk")
  [ "${#missing[@]}" -eq 0 ] || die "Android SDK components remain missing: ${missing[*]}"
}

ensure_platform_tools() {
  local archive install_root adb version
  configure_host_downloads
  install_root="$WORK_DIR/.tools/platform-tools-${PLATFORM_TOOLS_VERSION}-${HOST_TAG}"
  adb="$install_root/platform-tools/adb"
  if [ ! -f "$adb" ]; then
    archive="$(download_verified "Android Platform-Tools $PLATFORM_TOOLS_VERSION" \
      "$PLATFORM_TOOLS_URL" "$WORK_DIR/.downloads/$PLATFORM_TOOLS_ARCHIVE_NAME" \
      "$PLATFORM_TOOLS_SHA256")"
    remove_managed_tree "$install_root"
    mkdir -p "$install_root"
    unzip -q -o "$archive" -d "$install_root" >&2 || die "Android Platform-Tools extraction failed"
  fi
  [ -f "$adb" ] || die "Android Platform-Tools archive has an unexpected layout"
  chmod +x "$install_root/platform-tools"/* 2>/dev/null || true
  version="$("$adb" version 2>&1)" || die "pinned ADB failed to run: $version"
  printf '%s\n' "$version" | grep -Eq 'Version[[:space:]]+37[.]0[.]1-' ||
    die "pinned ADB version validation failed: $version"
  printf '%s\n' "$adb"
}

find_python() {
  local candidate
  for candidate in python3 python; do
    if command -v "$candidate" >/dev/null 2>&1 &&
       "$candidate" -c 'import sys; raise SystemExit(0 if sys.version_info >= (3, 10) else 1)' >/dev/null 2>&1; then
      command -v "$candidate"
      return 0
    fi
  done
  return 1
}

ensure_openxr_loader() {
  local aar output entry
  output="$WORK_DIR/.tools/openxr-1.1.43/libopenxr_loader.so"
  prepare_managed_directory "$(dirname "$output")"
  if [ -f "$output" ]; then
    if [ "$(sha256_file "$output")" = "$OPENXR_LOADER_SHA256" ]; then
      printf '%s\n' "$output"
      return 0
    fi
    rm -f -- "$output"
  fi
  aar="$(download_verified "Khronos OpenXR Android loader AAR 1.1.43" "$OPENXR_AAR_URL" \
    "$WORK_DIR/.downloads/openxr_loader_for_android-1.1.43.aar" "$OPENXR_AAR_SHA256")"
  entry="prefab/modules/openxr_loader/libs/android.arm64-v8a/libopenxr_loader.so"
  unzip -j -o "$aar" "$entry" -d "$(dirname "$output")" >/dev/null || die "OpenXR loader extraction failed"
  [ -f "$output" ] || die "OpenXR AAR did not contain the arm64 loader"
  [ "$(sha256_file "$output")" = "$OPENXR_LOADER_SHA256" ] || die "OpenXR loader payload hash mismatch"
  printf '%s\n' "$output"
}

ensure_vulkan_headers() {
  local archive install_root temporary headers
  install_root="$WORK_DIR/.tools/vulkan-headers-1.4.357.0"
  headers="$install_root/$VULKAN_HEADERS_ROOT_NAME"
  if [ ! -f "$headers/include/vulkan/vulkan_core.h" ]; then
    archive="$(download_verified "Vulkan-Headers 1.4.357.0" "$VULKAN_HEADERS_URL" \
      "$WORK_DIR/.downloads/$VULKAN_HEADERS_ARCHIVE" "$VULKAN_HEADERS_SHA256")"
    temporary="$WORK_DIR/.tools/.extract-vulkan-$RUN_ID"
    remove_managed_tree "$temporary"
    mkdir -p "$temporary"
    unzip -q -o "$archive" -d "$temporary" >&2 || die "Vulkan-Headers extraction failed"
    [ -f "$temporary/$VULKAN_HEADERS_ROOT_NAME/include/vulkan/vulkan_core.h" ] ||
      die "Vulkan-Headers archive has an unexpected layout"
    remove_managed_tree "$install_root"
    mv "$temporary" "$install_root" || die "could not install the Vulkan headers"
  fi
  [ -f "$headers/include/vulkan/vulkan_core.h" ] || die "Vulkan headers are incomplete: $headers"
  printf '%s\n' "$headers"
}

host_path() {
  if [ "$IS_MSYS" -eq 1 ] && command -v cygpath >/dev/null 2>&1; then
    cygpath -w "$1"
  else
    printf '%s\n' "$1"
  fi
}

local_path() {
  if [ "$IS_MSYS" -eq 1 ] && command -v cygpath >/dev/null 2>&1; then
    cygpath -u "$1"
  else
    printf '%s\n' "$1"
  fi
}

# Git Bash rewrites arguments that look like POSIX paths; device paths must
# reach ADB unchanged, so the conversion is switched off per call.
adb_global() {
  if [ "$IS_MSYS" -eq 1 ]; then MSYS_NO_PATHCONV=1 "$ADB" "$@"; else "$ADB" "$@"; fi
}

adb_cmd() {
  [ "${#ADB_BASE[@]}" -gt 0 ] || die "no Quest has been selected"
  if [ "$IS_MSYS" -eq 1 ]; then MSYS_NO_PATHCONV=1 "${ADB_BASE[@]}" "$@"; else "${ADB_BASE[@]}" "$@"; fi
}

verify_build_manifest() {
  local manifest="$1" summary
  mkdir -p "$PLAN_ROOT"
  summary="$("$PYTHON_EXE" - "$BUILD_ROOT" "$manifest" "$TRADE" "$MOD_VERSION" "$PLAN_ROOT" <<'PY' | tr -d '\r'
import hashlib, json, pathlib, re, sys

root = pathlib.Path(sys.argv[1]).resolve()
data = json.loads(pathlib.Path(sys.argv[2]).read_text(encoding="utf-8"))
trade, mod_version, plan = int(sys.argv[3]), sys.argv[4], pathlib.Path(sys.argv[5])
expected = {
    "formatVersion": 1, "modVersion": mod_version, "package": "com.rockstargames.rdr",
    "versionCode": "63226194", "versionName": "1.58.63226194",
}
for key, value in expected.items():
    if str(data.get(key)) != str(value):
        raise SystemExit(f"build manifest field {key!r} is unexpected: {data.get(key)!r}")
if str(data.get("engineSha256", "")).lower() != "3f80845edec32a663c42a8570e3131446813e695076c4bc96e2142c190bc22f3":
    raise SystemExit("build manifest engine hash is not the supported librdr.so")
if data.get("trade") is None or int(data["trade"]) != trade:
    raise SystemExit(f"build manifest trade value {data.get('trade')!r} does not match --trade {trade}")
if not re.fullmatch(r"[0-9a-fA-F]{64}", str(data.get("buildId", ""))):
    raise SystemExit("build manifest build id is missing")
if str(data.get("sourceSignerSha256", "")).upper() == "358020B0ED9D910C3EB4E03E261EB340604833F8BDC75E236D965E80E63A56DE":
    raise SystemExit("The game package was modified by a third party. Export the game from the phone with EXPORT_RDR_APKS.bat.")
if not re.fullmatch(r"[0-9a-fA-F]{64}", str(data.get("outputSignerSha256", ""))):
    raise SystemExit("output signer is missing from the build manifest")
code = str(data.get("outputSignerHashCode", ""))
if not re.fullmatch(r"[0-9a-fA-F]{1,8}", code):
    raise SystemExit("output signer hash code is missing from the build manifest")

def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()

out_root = (root / "out").resolve()
outputs = data.get("outputs") or []
if not outputs:
    raise SystemExit("build manifest contains no APK outputs")
bases = [r for r in outputs if r.get("split") is None]
if len(bases) != 1:
    raise SystemExit(f"build manifest must contain exactly one base APK, found {len(bases)}")
leaves, total = set(), 0
for record in outputs:
    split = record.get("split")
    leaf = "base.apk" if split is None else "split_" + re.sub(r"[^A-Za-z0-9_.-]+", "_", str(split)) + ".apk"
    if leaf in leaves:
        raise SystemExit(f"build manifest contains a duplicate APK split: {leaf}")
    leaves.add(leaf)
    path = (out_root / record["file"]).resolve()
    if out_root not in path.parents:
        raise SystemExit(f"manifest path escapes the output folder: {record['file']}")
    if not path.is_file() or path.stat().st_size != int(record["size"]):
        raise SystemExit(f"built APK size/path mismatch: {path}")
    print(f"Hashing {path.name}...", file=sys.stderr)
    if digest(path) != str(record["sha256"]).lower():
        raise SystemExit(f"built APK hash mismatch: {path}")
    total += path.stat().st_size
ordered = bases + sorted((r for r in outputs if r.get("split") is not None), key=lambda r: r["file"])
(plan / "apks.txt").write_text("".join(str((out_root / r["file"]).resolve()) + "\n" for r in ordered),
                               encoding="utf-8", newline="\n")
(plan / "expected-apks.tsv").write_text("".join(
    ("base.apk" if r.get("split") is None else "split_" + re.sub(r"[^A-Za-z0-9_.-]+", "_", str(r["split"])) + ".apk")
    + "\t" + str(r["sha256"]).lower() + "\n" for r in outputs), encoding="utf-8", newline="\n")
print(total, int(code, 16))
PY
)" || die "build verification failed"
  read -r APK_BYTES SIGNER_HASH_DEC <<< "$summary"
  echo "Verified build manifest: $manifest"
}

prepare_external_plan() {
  local manifest="$1"
  EXTERNAL_BYTES="$("$PYTHON_EXE" - "$manifest" "$PLAN_ROOT" "$PACKAGE_NAME" <<'PY' | tr -d '\r'
import hashlib, json, pathlib, re, sys

data = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
plan, package = pathlib.Path(sys.argv[2]), sys.argv[3]
external = data.get("external") or {}
root_value = external.get("root")
lines, grand = [], 0
safe = re.compile(r"[A-Za-z0-9_.+=-]+")
for name, nested in (("obb", package), ("assetpacks", "assetpacks")):
    records = [r for r in (external.get(name) or []) if r]
    if not records:
        continue
    if not root_value:
        raise SystemExit(f"the build manifest lists external {name} files but no export folder")
    root = pathlib.Path(root_value).resolve()
    candidates = [root, root / "external", root.parent]
    base = next((c for c in candidates if (c / name).is_dir()), None)
    if base is None:
        raise SystemExit(f"the external {name} folder of the game export was not found near {root}")
    content = base / name
    entries = list(content.iterdir())
    # The exporter pulls a device folder into external/<name>, which can add
    # one level named like the device folder.
    if len(entries) == 1 and entries[0].is_dir() and entries[0].name == nested:
        content = entries[0]
    local_files = sorted(p for p in content.rglob("*") if p.is_file())
    if len(local_files) != len(records):
        raise SystemExit(f"the external {name} folder changed after the build: {content}")
    used, sums, total = set(), [], 0
    for path in local_files:
        relative = path.relative_to(content).as_posix()
        if not all(safe.fullmatch(part) and part not in (".", "..") for part in relative.split("/")):
            raise SystemExit(f"external file name is unsafe for ADB: {path}")
        exact = [r for r in records if str(r["path"]).replace("\\", "/").lstrip("/") == relative]
        match = exact if len(exact) == 1 else [r for r in records if str(r["path"]).replace("\\", "/").endswith("/" + relative)]
        if len(match) != 1:
            raise SystemExit(f"external file is not listed in the build manifest: {path}")
        key = str(match[0]["path"])
        if key in used:
            raise SystemExit(f"external file is listed twice: {key}")
        used.add(key)
        if path.stat().st_size != int(match[0]["size"]):
            raise SystemExit(f"external file changed after the build: {path}")
        print(f"Hashing external {name} file {relative}...", file=sys.stderr)
        value = hashlib.sha256()
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                value.update(block)
        if value.hexdigest() != str(match[0]["sha256"]).lower():
            raise SystemExit(f"external file changed after the build: {path}")
        sums.append(f"{value.hexdigest()}  {relative}\n")
        total += path.stat().st_size
    if not safe.fullmatch(content.name):
        raise SystemExit(f"external folder name is unsafe for ADB: {content}")
    (plan / f"external-{name}.sha256").write_text("".join(sums), encoding="ascii", newline="\n")
    first = local_files[0].relative_to(content).as_posix()
    lines.append("\t".join((name, str(content), content.name, first, str(total))) + "\n")
    grand += total
(plan / "external.tsv").write_text("".join(lines), encoding="utf-8", newline="\n")
print(grand)
PY
)" || die "external game data verification failed"
}

show_connection_guidance() {
  echo ""
  echo "WAITING FOR THE QUEST HEADSET..."
  echo "  1. Developer Mode must be on: Meta Horizon phone app > Devices > (headset) > Headset settings > Developer mode."
  echo "     Restart the headset after switching it on."
  echo "  2. Connect the headset to this computer with a USB-C data cable. A charge-only cable is not detected."
  echo "  3. Put on the headset and accept Allow USB debugging. Tick Always allow from this computer."
  echo "The installer continues automatically when the headset is ready (up to 2 minutes)."
}

# Prints "<serial> <state>" for every device ADB reports.
list_adb_devices() {
  local output line serial state rest
  output="$(adb_global devices -l 2>&1)" || return 0
  while IFS= read -r line; do
    line="${line//$'\r'/}"
    [ -n "$line" ] || continue
    case "$line" in 'List of devices attached'*|'*'*) continue ;; esac
    read -r serial state rest <<< "$line"
    [ -n "$serial" ] && [ -n "$state" ] || continue
    printf '%s %s\n' "$serial" "$state"
  done <<< "$output"
}

wait_for_quest() {
  local timeout="${1:-120}" deadline now listing guidance=0 serial state
  local ready=()
  deadline=$(( $(date +%s) + timeout ))
  while true; do
    listing="$(list_adb_devices)"
    ready=()
    while read -r serial state; do
      [ -n "${serial:-}" ] || continue
      [ "$state" = "device" ] || continue
      if [ -z "$SERIAL" ] || [ "$serial" = "$SERIAL" ]; then ready+=("$serial"); fi
    done <<< "$listing"
    if [ "${#ready[@]}" -gt 0 ]; then
      READY_DEVICES=("${ready[@]}")
      return 0
    fi
    if [ "$guidance" -eq 0 ]; then
      show_connection_guidance
      guidance=1
    fi
    now="$(date +%s)"
    if [ "$now" -ge "$deadline" ]; then
      if [ -n "$SERIAL" ] && ! printf '%s\n' "$listing" | grep -q "^$SERIAL "; then
        die "the requested device $SERIAL was not found by ADB"
      fi
      if printf '%s\n' "$listing" | grep -Eq ' (unauthorized|authorizing)$'; then
        die "the headset is connected but USB debugging is not allowed yet. Put on the headset, accept Allow USB debugging (tick Always allow from this computer), then run the installer again"
      elif printf '%s\n' "$listing" | grep -Eq ' (offline|connecting)$'; then
        die "the headset connection is offline. Reconnect the USB cable or use another USB port, then run the installer again"
      elif printf '%s\n' "$listing" | grep -q ' no$'; then
        die "ADB has no permission to access the headset over USB. On Linux install the Android udev rules, add this account to the plugdev group, log in again and reconnect"
      fi
      die "no Quest headset was found over ADB. Check that Developer Mode is on, use a USB-C data cable and accept Allow USB debugging in the headset, then run the installer again"
    fi
    sleep 2
  done
}

select_quest() {
  local manufacturer model selection index message
  READY_DEVICES=()
  adb_global start-server >/dev/null 2>&1 || true
  wait_for_quest 120
  if [ -n "$SERIAL" ]; then
    :
  elif [ "${#READY_DEVICES[@]}" -eq 1 ]; then
    SERIAL="${READY_DEVICES[0]}"
  elif [ "$NON_INTERACTIVE" -eq 1 ]; then
    die "more than one ADB device is connected; pass --serial with --non-interactive"
  else
    echo "Connected ADB devices:"
    for ((index=0; index<${#READY_DEVICES[@]}; index++)); do echo "  [$((index+1))] ${READY_DEVICES[index]}"; done
    read -r -p "Choose the Quest headset by number: " selection
    [[ "$selection" =~ ^[0-9]+$ ]] || die "invalid device selection"
    [ "$selection" -ge 1 ] && [ "$selection" -le "${#READY_DEVICES[@]}" ] || die "invalid device selection"
    SERIAL="${READY_DEVICES[selection-1]}"
  fi
  ADB_BASE=("$ADB" -s "$SERIAL")
  model="$(adb_cmd shell getprop ro.product.model | tr -d '\r')" || die "could not read the device model"
  manufacturer="$(adb_cmd shell getprop ro.product.manufacturer | tr -d '\r')" ||
    die "could not read the device manufacturer"
  echo "Selected device: $SERIAL ($manufacturer $model)"
  if ! printf '%s\n' "$manufacturer" | grep -Eqi '^(oculus|meta)' ||
     ! printf '%s\n' "$model" | grep -Eqi '^quest[[:space:]]*3s?$'; then
    if printf '%s\n' "$manufacturer" | grep -Eqi '^(oculus|meta)'; then
      message="this headset reports the model '$model'. The mod supports Quest 3 and Quest 3S only; other headsets are untested and may be too slow"
    else
      message="the selected device does not identify itself as a Meta Quest 3 or Quest 3S: '$manufacturer $model'"
    fi
    [ "$NON_INTERACTIVE" -eq 0 ] || die "$message. Run the installer without --non-interactive to approve it"
    echo "WARNING: $message."
    read -r -p "Type INSTALL to continue: " selection
    [ "$selection" = "INSTALL" ] || die "installation was not approved for the selected device"
  fi
  case "$SERIAL" in
    *:*|*_adb-tls-connect*|*._tcp*)
      echo "WARNING: the headset is connected over Wi-Fi. Installing about 3 GB over Wi-Fi often fails when the connection drops; a USB-C data cable is more reliable."
      ;;
  esac
  echo "Keep the headset awake and connected until the installer finishes."
}

# Prints four lines: installed (0/1), signer state, versionCode, firstInstallTime.
installed_state() {
  local paths dump
  paths="$(adb_cmd shell pm path "$PACKAGE_NAME" 2>/dev/null | tr -d '\r')" || paths=""
  dump=""
  if printf '%s\n' "$paths" | grep -q '^package:'; then
    dump="$(adb_cmd shell dumpsys package "$PACKAGE_NAME" 2>/dev/null | tr -d '\r')" || dump=""
  fi
  printf '%s\n' "$paths" > "$PLAN_ROOT/pm-path.txt"
  printf '%s\n' "$dump" > "$PLAN_ROOT/dumpsys-package.txt"
  "$PYTHON_EXE" - "$PLAN_ROOT/pm-path.txt" "$PLAN_ROOT/dumpsys-package.txt" "$SIGNER_HASH_DEC" <<'PY'
import pathlib, re, sys
paths = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace")
dump = pathlib.Path(sys.argv[2]).read_text(encoding="utf-8", errors="replace")
ours = int(sys.argv[3])
installed = any(line.startswith("package:") for line in paths.splitlines())
version = re.search(r"(?m)^\s*versionCode=(\d+)", dump)
first = re.search(r"(?m)^\s*firstInstallTime=(.+?)\s*$", dump)
signers = re.search(r"(?<!past )signatures:\[([0-9a-fA-F, ]*)\]", dump)
hashes = [int(v, 16) for v in signers.group(1).replace(" ", "").split(",") if v] if signers else []
if not installed:
    state = "none"
elif not hashes:
    state = "unknown"
elif hashes == [ours]:
    state = "match"
else:
    state = "foreign"
print(1 if installed else 0)
print(state)
print(version.group(1) if version else "")
print(first.group(1) if first else "")
PY
}

stop_game_and_verify() {
  local process_output
  adb_cmd shell am force-stop "$PACKAGE_NAME" >/dev/null || die "could not force-stop the game"
  process_output="$(adb_cmd shell pidof "$PACKAGE_NAME" 2>/dev/null || true)"
  process_output="${process_output//$'\r'/}"
  [ -z "$process_output" ] || die "the game is still running after force-stop (PID $process_output)"
}

assert_quest_free_space() {
  local required_kib df_output available_kib
  # Include staging, installed files, native extraction and Android's reserve
  # for fresh installs as well as updates.
  required_kib=$(( (2 * APK_BYTES + 3221225472 + EXTERNAL_BYTES + 1023) / 1024 ))
  df_output="$(adb_cmd shell toybox df -k /sdcard)" || die "could not query the free storage of the headset"
  available_kib="$(printf '%s\n' "$df_output" | tr -d '\r' | awk 'NR > 1 && NF >= 4 {value=$4} END {print value}')"
  [[ "$available_kib" =~ ^[0-9]+$ ]] || die "could not parse the free storage of the headset"
  awk -v have="$available_kib" -v need="$required_kib" \
    'BEGIN {printf "Quest free space: %.2f GiB; required: %.2f GiB\n", have/1048576, need/1048576}'
  [ "$available_kib" -ge "$required_kib" ] ||
    die "the headset does not have enough free storage. Free more space (uninstall unused apps or delete videos), then run the installer again"
}

remote_sha256() {
  local remote="$1" hash
  case "$remote" in *"'"*) die "unsafe remote path: $remote" ;; esac
  hash="$(adb_cmd shell "toybox sha256sum '$remote'" | tr -d '\r' | awk '{print tolower($1); exit}')" ||
    die "could not hash $remote on the headset"
  [[ "$hash" =~ ^[0-9a-f]{64}$ ]] || die "unexpected sha256sum output for $remote"
  printf '%s\n' "$hash"
}

verify_remote_tree() {
  local remote="$1" sums="$2"
  adb_cmd shell "cd '$remote' && toybox sha256sum -c '$sums' >/dev/null"
}

assert_safe_generated_path() {
  [[ "$1" =~ ^/sdcard/(\.rdrvr-stage-[a-f0-9]{32}(/[A-Za-z0-9_.-]+)?|Android/obb/\.rdrvr-backup-[a-f0-9]{32}|Android/data/com\.rockstargames\.rdr/files/\.rdrvr-backup-[a-f0-9]{32})$ ]] ||
    die "refusing unsafe generated remote path: $1"
}

new_remote_parent() {
  local parent="$1" current component rest
  local components=()
  [[ "$parent" =~ ^/sdcard/Android/[A-Za-z0-9_./-]+$ ]] && [[ "$parent" != *..* ]] ||
    die "refusing an unexpected remote parent: $parent"
  current="/sdcard/Android"
  rest="${parent#/sdcard/Android/}"
  IFS='/' read -r -a components <<< "$rest"
  for component in "${components[@]}"; do
    current="$current/$component"
    if adb_cmd shell test -e "$current" >/dev/null 2>&1; then continue; fi
    adb_cmd shell mkdir -p "$current" || die "could not create $current"
    # Shell-created folders must stay writable for the game, which creates
    # its own folders inside them on the first start.
    adb_cmd shell chmod a+rwx "$current" ||
      echo "WARNING: could not open permissions on $current"
  done
}

publish_external_tree() {
  local name="$1" content="$2" leaf="$3" parent="$4" target="$5" first="$6" total="$7"
  local id stage_container stage sums_remote backup had_existing=0 rollback_index local_dir relative_dir path
  case "$target" in "$REMOTE_OBB_ROOT"|"$REMOTE_FILES_ROOT/assetpacks") ;; *) die "refusing unexpected final path: $target" ;; esac
  id="$("$PYTHON_EXE" -c 'import uuid; print(uuid.uuid4().hex)' | tr -d '\r')"
  # Never use adb push below Android/data or Android/obb. Upload and verify
  # on ordinary shared storage, then move the verified tree with the device shell.
  stage_container="/sdcard/.rdrvr-stage-$id"
  stage="$stage_container/$leaf"
  sums_remote="$stage_container/$name.sha256"
  backup="$parent/.rdrvr-backup-$id"
  for path in "$stage_container" "$stage" "$sums_remote" "$backup"; do assert_safe_generated_path "$path"; done
  REMOTE_STAGING+=("$stage_container")
  echo "==> Uploading external $name data ($total bytes)"
  new_remote_parent "$parent"
  adb_cmd shell rm -rf "$stage_container" || die "could not prepare the staging folder"
  adb_cmd shell mkdir -p "$stage" || die "could not create the staging folder"
  while IFS= read -r -d '' local_dir; do
    relative_dir="${local_dir#"$content"/}"
    [[ "$relative_dir" =~ ^[A-Za-z0-9_.+=-]+(/[A-Za-z0-9_.+=-]+)*$ ]] ||
      die "directory name is unsafe for ADB: $relative_dir"
    adb_cmd shell mkdir -p "$stage/$relative_dir" || die "could not create a staged directory: $relative_dir"
  done < <(find "$content" -mindepth 1 -type d -print0)
  # Pushing a folder into an existing remote folder creates <remote>/<leaf>.
  adb_cmd push "$(host_path "$content")" "$stage_container" || die "external $name transfer failed"
  adb_cmd push "$(host_path "$PLAN_ROOT/external-$name.sha256")" "$sums_remote" >/dev/null ||
    die "external $name checksum transfer failed"
  adb_cmd shell test -f "$stage/$first" || die "external $name transfer created an unexpected layout"
  verify_remote_tree "$stage" "$sums_remote" || die "hash verification on the headset failed for staged $name data"
  if adb_cmd shell test -d "$target" >/dev/null 2>&1; then
    had_existing=1
  elif adb_cmd shell test -e "$target" >/dev/null 2>&1; then
    die "existing $name target is not a directory: $target"
  fi
  rollback_index="${#REMOTE_ROLLBACK_TARGETS[@]}"
  REMOTE_ROLLBACK_TARGETS+=("$target")
  REMOTE_ROLLBACK_BACKUPS+=("$backup")
  REMOTE_ROLLBACK_HAD_EXISTING+=("$had_existing")
  if [ "$had_existing" -eq 1 ]; then
    adb_cmd shell mv "$target" "$backup" || die "could not move the existing $name data aside"
  fi
  adb_cmd shell mv "$stage" "$target" || die "could not activate the $name data; rollback will be attempted"
  # Files written by the ADB shell keep shell ownership after the move;
  # the game needs read and write access to its own data.
  adb_cmd shell chmod -R a+rwX "$target" ||
    die "could not make the $name data accessible to the game"
  verify_remote_tree "$target" "$sums_remote" ||
    die "final hash verification failed for $name data; rollback will be attempted"
  REMOTE_ROLLBACK_TARGETS[rollback_index]=""
  REMOTE_ROLLBACK_BACKUPS[rollback_index]=""
  REMOTE_ROLLBACK_HAD_EXISTING[rollback_index]=""
  if ! adb_cmd shell rm -rf "$stage_container" >/dev/null 2>&1; then
    echo "WARNING: the $name data is active, but the staging folder remains: $stage_container"
  fi
  if [ "$had_existing" -eq 1 ]; then
    adb_cmd shell rm -rf "$backup" || die "the $name data is active, but the previous copy could not be removed: $backup"
  fi
  echo "Published and verified: $target"
}

backup_quest_files() {
  local backup="$1" skip_assetpacks="$2" remote_root="$REMOTE_FILES_ROOT" listing name rc
  local relative expected remote_hash
  mkdir -p "$backup/files"
  : > "$backup/top-level.txt"
  if adb_cmd shell test -d "$remote_root" >/dev/null 2>&1; then
    set +e
    listing="$(adb_cmd shell ls -1A "$remote_root" 2>&1)"
    rc="$?"
    set -e
    [ "$rc" -eq 0 ] || die "could not list the game files on the headset: $listing"
  else
    listing=""
  fi
  while IFS= read -r name; do
    name="${name//$'\r'/}"
    [ -n "$name" ] || continue
    case "$name" in .rdrvr-stage-*|.rdrvr-backup-*) continue ;; esac
    if [ "$name" = "assetpacks" ] && [ "$skip_assetpacks" -eq 1 ]; then continue; fi
    if [ "$name" = "." ] || [ "$name" = ".." ] || [[ ! "$name" =~ ^[A-Za-z0-9_.+=-]+$ ]]; then
      die "unsafe file name in the game folder on the headset: $name"
    fi
    echo "Backing up $remote_root/$name"
    adb_cmd pull "$remote_root/$name" "$(host_path "$backup/files")" >/dev/null ||
      die "could not back up $remote_root/$name"
    printf '%s\n' "$name" >> "$backup/top-level.txt"
  done <<< "$listing"
  "$PYTHON_EXE" - "$backup" "$remote_root" "$skip_assetpacks" <<'PY'
import hashlib, json, pathlib, sys
root, remote_root, skip = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3] == "1"
files_root = (root / "files").resolve()
items = []
for path in sorted((root / "files").rglob("*")):
    if path.is_symlink():
        raise SystemExit(f"refusing a symlink in the backup: {path}")
    if not path.is_file():
        continue
    if files_root not in path.resolve().parents:
        raise SystemExit(f"backup path escapes the backup folder: {path}")
    relative = path.relative_to(root / "files").as_posix()
    if any(c in relative for c in ("'", "\t", "\r", "\n")):
        raise SystemExit(f"unsafe backed-up relative path: {relative!r}")
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    items.append({"path": relative, "size": path.stat().st_size, "sha256": value.hexdigest()})
top = [line for line in (root / "top-level.txt").read_text(encoding="utf-8").splitlines() if line]
(root / "file-manifest.tsv").write_text("".join(f'{i["path"]}\t{i["sha256"]}\n' for i in items),
                                       encoding="utf-8", newline="\n")
excluded = [".rdrvr-stage-*", ".rdrvr-backup-*"] + (["assetpacks"] if skip else [])
(root / "backup-manifest.json").write_text(json.dumps({
    "formatVersion": 1, "package": "com.rockstargames.rdr", "remoteRoot": remote_root,
    "excluded": excluded, "topLevelItems": top, "files": items}, indent=2) + "\n", encoding="utf-8", newline="\n")
PY
  while IFS=$'\t' read -r relative expected; do
    [ -n "$relative" ] || continue
    remote_hash="$(remote_sha256 "$remote_root/$relative")"
    [ "$remote_hash" = "$expected" ] || die "backup hash mismatch: $relative"
  done < "$backup/file-manifest.tsv"
  echo "Backup verified: $backup"
}

replace_foreign_install() {
  local skip_assetpacks="$1" answer output rc
  [ "$NON_INTERACTIVE" -eq 0 ] ||
    die "the installed game was signed with a different key. Replacing it requires uninstalling it, which needs typed confirmation. Run the installer without --non-interactive. The installed game was left in place"
  DEVICE_BACKUP="$WORK_DIR/device-backups/$RUN_ID"
  echo "Backing up the game files that ADB can read (settings and reports)..."
  backup_quest_files "$DEVICE_BACKUP" "$skip_assetpacks"
  echo ""
  echo "WARNING: the installed copy was signed with a different key than this personal build."
  echo "Android accepts this build only after the installed copy is uninstalled."
  echo "Uninstalling deletes the saved games, the Rockstar first-start acceptance and the game folders on the headset."
  echo "Saved games are stored in private app storage and cannot be copied over ADB, so they cannot be backed up here."
  echo "Backup of the ADB-readable files: $DEVICE_BACKUP"
  read -r -p "Type UNINSTALL to continue: " answer
  [ "$answer" = "UNINSTALL" ] || die "uninstall was not approved. The installed game was left in place. Backup: $DEVICE_BACKUP"
  stop_game_and_verify
  set +e
  output="$(adb_cmd uninstall "$PACKAGE_NAME" 2>&1)"
  rc="$?"
  set -e
  printf '%s\n' "$output"
  [ "$rc" -eq 0 ] && printf '%s\n' "$output" | tr -d '\r' | grep -Eq '^Success[[:space:]]*$' ||
    die "the approved uninstall failed"
  echo "The previous copy was uninstalled."
}

# Sets INSTALL_RESULT to success, mismatch or failed.
install_apks() {
  local attempt output rc apk
  local args=()
  local apks=()
  while IFS= read -r apk; do
    [ -n "$apk" ] && apks+=("$(host_path "$apk")")
  done < "$PLAN_ROOT/apks.txt"
  [ "${#apks[@]}" -gt 0 ] || die "no assembled APKs were found"
  if [ "${#apks[@]}" -eq 1 ]; then
    args=(install -r --no-incremental "${apks[0]}")
  else
    args=(install-multiple -r --no-incremental "${apks[@]}")
  fi
  for attempt in 1 2; do
    echo "Sending the APK set to the headset. This takes a few minutes over USB."
    set +e
    output="$(adb_cmd "${args[@]}" 2>&1)"
    rc="$?"
    set -e
    printf '%s\n' "$output"
    output="${output//$'\r'/}"
    if [ "$rc" -eq 0 ] && printf '%s\n' "$output" | grep -Eq '^Success[[:space:]]*$'; then
      INSTALL_RESULT="success"
      return 0
    fi
    if printf '%s\n' "$output" | grep -qi 'INSTALL_FAILED_INSUFFICIENT_STORAGE'; then
      die "Android could not allocate enough storage for the installation. Free more space on the headset (unused apps or videos), then retry. The APK build succeeded; keep the build outputs and personal signing key"
    fi
    if printf '%s\n' "$output" | grep -Eqi "INSTALL_FAILED_UPDATE_INCOMPATIBLE|signatures? do not match|inconsistent certificates"; then
      INSTALL_RESULT="mismatch"
      return 0
    fi
    if [ "$attempt" -eq 1 ] &&
       printf '%s\n' "$output" | grep -Eqi "device offline|device '[^']*' not found|device not found|no devices/emulators found|broken pipe|connection reset|protocol fault|failed to (read|write)|closed|transport"; then
      echo "WARNING: the connection to the headset was interrupted. Reconnect the cable if needed and keep the headset awake; the installer retries once."
      wait_for_quest 120
      continue
    fi
    break
  done
  INSTALL_RESULT="failed"
}

verify_installed_apks() {
  local output plan leaf remote expected observed state line
  output="$(adb_cmd shell pm path "$PACKAGE_NAME" | tr -d '\r')" || die "pm path failed after installation"
  printf '%s\n' "$output" > "$PLAN_ROOT/pm-path-installed.txt"
  plan="$("$PYTHON_EXE" - "$PLAN_ROOT/expected-apks.tsv" "$PLAN_ROOT/pm-path-installed.txt" <<'PY' | tr -d '\r'
import pathlib, re, sys
expected = {}
for line in pathlib.Path(sys.argv[1]).read_text(encoding="utf-8").splitlines():
    if line:
        leaf, digest = line.split("\t")
        expected[leaf] = digest
remote = {}
for line in pathlib.Path(sys.argv[2]).read_text(encoding="utf-8").splitlines():
    if not line:
        continue
    if not line.startswith("package:") or not line.endswith(".apk"):
        raise SystemExit(f"unexpected pm path output: {line!r}")
    path = line[len("package:"):]
    if not re.fullmatch(r"/data/app/[A-Za-z0-9_.~=+/-]+\.apk", path) or ".." in path:
        raise SystemExit(f"unexpected installed APK path: {path}")
    leaf = path.rsplit("/", 1)[1]
    if leaf in remote:
        raise SystemExit(f"installed package reports a duplicate APK: {leaf}")
    remote[leaf] = path
if sorted(remote) != sorted(expected):
    raise SystemExit(f"installed APK set differs from the build; expected {sorted(expected)}, got {sorted(remote)}")
for leaf in sorted(expected):
    print(f"{leaf}\t{remote[leaf]}\t{expected[leaf]}")
PY
)" || die "the installed APK set does not match the build"
  while IFS=$'\t' read -r leaf remote expected; do
    [ -n "$leaf" ] || continue
    echo "Hashing the installed $leaf on the headset..."
    observed="$(remote_sha256 "$remote")"
    [ "$observed" = "$expected" ] || die "installed APK hash mismatch: $leaf"
  done <<< "$plan"
  state="$(installed_state | tr -d '\r')"
  INSTALLED_AFTER=()
  while IFS= read -r line; do INSTALLED_AFTER+=("$line"); done <<< "$state"
  [ "${INSTALLED_AFTER[2]-}" = "$EXPECTED_VERSION_CODE" ] ||
    die "the installed versionCode is ${INSTALLED_AFTER[2]-unknown}, expected $EXPECTED_VERSION_CODE"
  case "${INSTALLED_AFTER[1]-}" in
    match) ;;
    unknown) echo "WARNING: could not read the installed signer from dumpsys; the APK hashes above are verified." ;;
    *) die "the installed signer is not the personal signing key of this build" ;;
  esac
  echo "Installed APKs verified: versionCode $EXPECTED_VERSION_CODE, personal signer."
}

print_first_launch_hints() {
  echo ""
  echo "FIRST START"
  echo "  - In the headset open the Library, set the filter to Unknown sources and start RDR."
  echo "  - Start the game with the headset on, in a lit room, with tracking active. If the headset asks to turn"
  echo "    on tracking, turn it on; do not continue without tracking (6DoF is required)."
  echo "  - The first start shows the Rockstar privacy policy and license screens once (thumbstick scrolls, A continues)."
  echo "  - VR menu: hold the menu button on the left controller. Settings are saved automatically."
  echo "  - If the headset reports that RDR is not responding, choose Close app and start the game again."
  echo ""
  echo "SIGNING KEY"
  echo "  $KEYSTORE"
  echo "  Keep this file and a backup of it. Updates must be signed with the same key. Without it the game has"
  echo "  to be uninstalled before the next install, and saved games are lost."
}

# Optional last step: starts the GPU boost helper. A failure only prints a note.
start_gpu_boost_helper() {
  local helper="$ROOT/tools/rdrvr_boost.sh" remote="/data/local/tmp/rdrvr_boost.sh" status_text attempt
  echo "==> Starting the GPU boost helper"
  if [ -f "$helper" ] &&
     adb_cmd push "$(host_path "$helper")" "$remote" >/dev/null 2>&1 &&
     adb_cmd shell sh "$remote" start >/dev/null 2>&1; then
    for attempt in 1 2 3 4 5; do
      sleep 2
      status_text="$(adb_cmd shell sh "$remote" status 2>/dev/null | tr -d '\r' || true)"
      case "$status_text" in
        "helper: running"*)
          echo "GPU boost helper started. Run GPU_BOOST again after every headset restart."
          return 0
          ;;
      esac
    done
  fi
  echo "NOTE: the GPU boost helper was not started. The install is complete;"
  echo "      run GPU_BOOST later to use GPU BOOST in the game."
  return 0
}

cleanup_on_exit() {
  local rc="$?" path index target backup had_existing
  set +e
  if [ "$INSTALL_ACTIVE" -eq 1 ] && [ "${#ADB_BASE[@]}" -gt 0 ]; then
    if [ "$rc" -ne 0 ]; then
      for ((index=0; index<${#REMOTE_ROLLBACK_TARGETS[@]}; index++)); do
        target="${REMOTE_ROLLBACK_TARGETS[index]-}"
        backup="${REMOTE_ROLLBACK_BACKUPS[index]-}"
        had_existing="${REMOTE_ROLLBACK_HAD_EXISTING[index]-}"
        [ -n "$target" ] && [ -n "$backup" ] || continue
        case "$had_existing" in 0|1) ;; *) continue ;; esac
        if [ "$had_existing" -eq 1 ]; then
          if adb_cmd shell test -d "$backup" >/dev/null 2>&1; then
            if adb_cmd shell test -e "$target" >/dev/null 2>&1 &&
               ! adb_cmd shell rm -rf "$target" >/dev/null 2>&1; then
              echo "WARNING: could not remove interrupted data: $target"
              echo "WARNING: the previous copy remains at: $backup"
              continue
            fi
            if adb_cmd shell mv "$backup" "$target" >/dev/null 2>&1; then
              echo "Restored the previous data at: $target"
            else
              echo "WARNING: could not restore the previous data; it remains at: $backup"
            fi
          fi
        elif adb_cmd shell test -e "$target" >/dev/null 2>&1; then
          if adb_cmd shell rm -rf "$target" >/dev/null 2>&1; then
            echo "Removed interrupted data that had no previous copy: $target"
          else
            echo "WARNING: could not remove interrupted data: $target"
          fi
        fi
      done
    fi
    if [ "${#REMOTE_STAGING[@]}" -gt 0 ]; then
      for path in "${REMOTE_STAGING[@]}"; do
        adb_cmd shell rm -rf "$path" >/dev/null 2>&1 || true
      done
    fi
    adb_cmd shell am force-stop "$PACKAGE_NAME" >/dev/null 2>&1 || true
  fi
  if [ -n "$KEY_LOCK" ]; then
    rmdir "$KEY_LOCK" >/dev/null 2>&1 || true
  fi
  if [ -n "$KEY_TEMP" ] && [ -n "$SIGNING_ROOT" ] && path_is_within "$KEY_TEMP" "$SIGNING_ROOT"; then
    rm -f "$KEY_TEMP" >/dev/null 2>&1 || true
  fi
  if [ "$rc" -ne 0 ]; then
    echo "FAILED. Read the error above."
    [ -n "$LOG_PATH" ] && echo "Diagnostic log: $LOG_PATH"
  fi
}

prepare_keystore() {
  local data_home signing_candidate
  if [ -n "$KEYSTORE_ARG" ]; then
    KEYSTORE="$(normalize_prompt_path "$KEYSTORE_ARG")"
    [ -f "$KEYSTORE" ] || die "requested signing key does not exist: $KEYSTORE"
    KEYSTORE="$(absolute_existing "$KEYSTORE")"
    BUILD_KEYSTORE="$KEYSTORE"
    return 0
  fi
  data_home="$(normalize_prompt_path "${XDG_DATA_HOME:-${HOME}/.local/share}")"
  signing_candidate="$data_home/rdrvr-builder/signing"
  [ ! -L "$signing_candidate" ] || die "persistent signing directory must not be a symlink: $signing_candidate"
  SIGNING_ROOT="$(canonicalize_directory_target "$signing_candidate")"
  if path_is_within "$SIGNING_ROOT" "$ROOT" || path_is_within "$SIGNING_ROOT" "$WORK_DIR"; then
    die "persistent signing directory must stay outside the source kit and --work-dir: $SIGNING_ROOT"
  fi
  mkdir -p "$SIGNING_ROOT"
  [ ! -L "$SIGNING_ROOT" ] || die "persistent signing directory must not be a symlink: $SIGNING_ROOT"
  [ "$(cd "$SIGNING_ROOT" && pwd -P)" = "$SIGNING_ROOT" ] || die "persistent signing directory resolved unexpectedly"
  [ -O "$SIGNING_ROOT" ] || die "persistent signing directory must be owned by the current account: $SIGNING_ROOT"
  chmod 700 "$SIGNING_ROOT" || die "could not protect the persistent signing directory"
  KEYSTORE="$SIGNING_ROOT/rdrvr.keystore"
  [ ! -L "$KEYSTORE" ] || die "persistent signing key must not be a symlink: $KEYSTORE"
  if [ -e "$KEYSTORE" ]; then
    [ -f "$KEYSTORE" ] || die "persistent signing key is not a regular file: $KEYSTORE"
    [ -O "$KEYSTORE" ] || die "persistent signing key must be owned by the current account: $KEYSTORE"
    chmod 600 "$KEYSTORE" || die "could not protect the persistent signing key"
    BUILD_KEYSTORE="$KEYSTORE"
  else
    KEY_LOCK="$SIGNING_ROOT/.keystore-create.lock"
    mkdir "$KEY_LOCK" ||
      die "signing-key lock exists: $KEY_LOCK. If no builder is running, remove only this empty lock directory and retry"
    KEY_TEMP="$SIGNING_ROOT/.rdrvr.keystore.new-$RUN_ID"
    [ ! -e "$KEY_TEMP" ] && [ ! -L "$KEY_TEMP" ] || die "temporary signing key path already exists: $KEY_TEMP"
    BUILD_KEYSTORE="$KEY_TEMP"
  fi
}

publish_keystore() {
  [ -f "$BUILD_KEYSTORE" ] && [ ! -L "$BUILD_KEYSTORE" ] || die "the build did not produce a regular signing key"
  [ -O "$BUILD_KEYSTORE" ] || die "the signing key must be owned by the current account"
  chmod 600 "$BUILD_KEYSTORE" || die "could not protect the signing key"
  if [ -n "$KEY_TEMP" ]; then
    [ ! -e "$KEYSTORE" ] || die "a persistent signing key appeared while the builder held its creation lock"
    mv "$KEY_TEMP" "$KEYSTORE" || die "could not publish the persistent signing key"
    KEY_TEMP=""
    chmod 600 "$KEYSTORE" || die "could not protect the persistent signing key"
  fi
  if [ -n "$KEY_LOCK" ]; then
    rmdir "$KEY_LOCK" || die "could not release the signing-key creation lock: $KEY_LOCK"
    KEY_LOCK=""
  fi
}

main() {
  local managed_name prompted=0 tool state_lines mode name content leaf parent final first total line
  local skip_assetpacks=0 update_space=0
  local before=()
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --game-package|-GamePackage) GAME_PACKAGE="${2:?missing game package}"; shift 2 ;;
      --work-dir|-WorkDir) WORK_DIR="${2:?missing work directory}"; shift 2 ;;
      --android-sdk|-AndroidSdk) ANDROID_SDK="${2:?missing Android SDK}"; shift 2 ;;
      --java-home|-JavaHome) JAVA_HOME_ARG="${2:?missing Java home}"; shift 2 ;;
      --serial|-Serial) SERIAL="${2:?missing serial}"; shift 2 ;;
      --log-path|-LogPath) LOG_PATH="${2:?missing log path}"; shift 2 ;;
      --keystore|-Keystore) KEYSTORE_ARG="${2:?missing keystore}"; shift 2 ;;
      --trade|-Trade) TRADE="${2:?missing trade value}"; shift 2 ;;
      --build-only|-BuildOnly) BUILD_ONLY=1; shift ;;
      --non-interactive|-NonInteractive) NON_INTERACTIVE=1; shift ;;
      -h|--help) usage; exit 0 ;;
      *) die "unknown argument: $1 (run --help)" ;;
    esac
  done
  case "$TRADE" in -1|0|1) ;; *) die "--trade must be -1, 0 or 1" ;; esac

  PYTHON_EXE="$(find_python)" || die "Python 3.10 or newer is required (python3)"
  for tool in curl unzip tar; do
    command -v "$tool" >/dev/null 2>&1 || die "$tool is required"
  done
  [ -f "$ROOT/tools/build.py" ] || die "the source kit is incomplete; missing $ROOT/tools/build.py"
  WORK_DIR="$(normalize_prompt_path "$WORK_DIR")"
  [ -n "$WORK_DIR" ] || die "--work-dir must be a dedicated directory"
  WORK_DIR="$(canonicalize_directory_target "$WORK_DIR")"
  [ "$WORK_DIR" != "/" ] || die "--work-dir resolved to a filesystem root"
  if path_is_within "$WORK_DIR" "$ROOT" || path_is_within "$ROOT" "$WORK_DIR"; then
    die "--work-dir and the source kit must be separate, non-nested directories"
  fi
  mkdir -p "$WORK_DIR"
  WORK_DIR="$(cd "$WORK_DIR" && pwd -P)"
  for managed_name in .downloads .tools .android-sdk runs device-backups logs; do
    prepare_managed_directory "$WORK_DIR/$managed_name"
  done
  STAMP="$(date +%Y%m%d-%H%M%S)"
  RUN_ID="$STAMP-$$-${RANDOM}${RANDOM}"
  BUILD_ROOT="$WORK_DIR/runs/$RUN_ID"
  PLAN_ROOT="$BUILD_ROOT/install-plan"
  prepare_managed_directory "$BUILD_ROOT"
  if [ -z "$LOG_PATH" ]; then
    LOG_PATH="$WORK_DIR/logs/build-and-install-$RUN_ID.log"
  else
    LOG_PATH="$(normalize_prompt_path "$LOG_PATH")"
  fi
  mkdir -p "$(dirname "$LOG_PATH")"
  exec > >(tee -a "$LOG_PATH") 2>&1
  trap cleanup_on_exit EXIT

  echo "Red Dead Redemption VR $MOD_VERSION - personal Quest builder"
  echo "This tool needs your own Red Dead Redemption $EXPECTED_VERSION_NAME for Android, exported from your phone"
  echo "with EXPORT_RDR_APKS.bat on a Windows PC (the folder with base.apk and the split_*.apk files)."
  echo "It does not download or redistribute game APKs or game data."
  echo "Work directory: $WORK_DIR"
  echo "Log: $LOG_PATH"

  if [ -z "$GAME_PACKAGE" ]; then
    [ "$NON_INTERACTIVE" -eq 0 ] && [ -t 0 ] || die "--game-package is required in non-interactive mode"
    echo "Game input: the export folder that contains base.apk and the split_*.apk files, or a single APK."
    read -r -p "Path of the game export: " GAME_PACKAGE
    prompted=1
  fi
  if [ "$prompted" -eq 1 ]; then GAME_PACKAGE="$(normalize_prompt_path "$GAME_PACKAGE")"; fi
  [ -e "$GAME_PACKAGE" ] || die "game input does not exist: $GAME_PACKAGE"
  GAME_PACKAGE="$(absolute_existing "$GAME_PACKAGE")"
  echo "Game input: $GAME_PACKAGE"
  prepare_keystore

  echo "==> Preparing pinned build tools"
  JAVA_HOME_ARG="$(find_java_home)"
  export JAVA_HOME="$JAVA_HOME_ARG"
  ANDROID_SDK="$(find_android_sdk)"
  export ANDROID_HOME="$ANDROID_SDK"
  export ANDROID_SDK_ROOT="$ANDROID_SDK"
  ensure_sdk_packages "$ANDROID_SDK"
  OPENXR_LOADER="$(ensure_openxr_loader)"
  VULKAN_HEADERS="$(ensure_vulkan_headers)"
  echo "JDK: $JAVA_HOME_ARG"
  echo "Android SDK: $ANDROID_SDK"
  echo "Python: $PYTHON_EXE"
  echo "Personal signing key: $KEYSTORE (created by the first build; keep it for future updates)"

  echo "==> Building the personal APK set"
  "$PYTHON_EXE" -u "$ROOT/tools/build.py" \
    --game-package "$GAME_PACKAGE" \
    --build-dir "$BUILD_ROOT" \
    --sdk "$ANDROID_SDK" \
    --java-home "$JAVA_HOME_ARG" \
    --openxr-loader "$OPENXR_LOADER" \
    --vulkan-headers "$VULKAN_HEADERS" \
    --keystore "$BUILD_KEYSTORE" \
    --trade "$TRADE" ||
    die "the build failed. Read the ERROR line above; step logs are in $BUILD_ROOT/logs"
  publish_keystore

  echo "==> Verifying the build outputs"
  [ -f "$BUILD_ROOT/build-manifest.json" ] || die "the build did not produce build-manifest.json"
  verify_build_manifest "$BUILD_ROOT/build-manifest.json"
  if [ "$BUILD_ONLY" -eq 1 ]; then
    echo ""
    echo "BUILD AND LOCAL VERIFICATION COMPLETED."
    echo "APK output: $BUILD_ROOT/out"
    echo "Build manifest: $BUILD_ROOT/build-manifest.json"
    echo "No device was selected or modified."
    echo "Diagnostic log: $LOG_PATH"
    exit 0
  fi
  prepare_external_plan "$BUILD_ROOT/build-manifest.json"

  echo "==> Selecting the Quest headset"
  ADB="$(ensure_platform_tools)"
  [ -x "$ADB" ] || die "pinned ADB was not installed at $ADB"
  select_quest

  echo "==> Checking the installed game"
  state_lines="$(installed_state | tr -d '\r')"
  while IFS= read -r line; do before+=("$line"); done <<< "$state_lines"
  mode="${before[1]-none}"
  case "$mode" in
    none) echo "Red Dead Redemption is not installed on this headset. A fresh install follows." ;;
    match) echo "The installed copy uses this signing key. It is updated in place; saved games and settings are kept."; update_space=1 ;;
    unknown) echo "WARNING: could not read the signing key of the installed copy; the install result decides."; update_space=1 ;;
    foreign) echo "WARNING: the installed copy was signed with a different key." ;;
  esac
  if [ "${before[0]-0}" = "1" ] && [ -n "${before[2]-}" ] && [ "${before[2]}" != "$EXPECTED_VERSION_CODE" ]; then
    echo "WARNING: the installed copy reports versionCode ${before[2]}; this build is $EXPECTED_VERSION_CODE."
  fi
  INSTALL_ACTIVE=1
  stop_game_and_verify
  assert_quest_free_space "$update_space"
  if grep -q '^assetpacks	' "$PLAN_ROOT/external.tsv" 2>/dev/null; then skip_assetpacks=1; fi
  if [ "$mode" = "foreign" ]; then
    replace_foreign_install "$skip_assetpacks"
  fi

  echo "==> Installing the personally signed APK set"
  install_apks
  if [ "$INSTALL_RESULT" = "mismatch" ] && [ -z "$DEVICE_BACKUP" ]; then
    echo "WARNING: Android rejected the update because the installed copy has a different signing key."
    replace_foreign_install "$skip_assetpacks"
    mode="foreign"
    install_apks
  fi
  if [ "$INSTALL_RESULT" != "success" ]; then
    if [ -n "$DEVICE_BACKUP" ]; then die "APK installation failed. Read the ADB message above. Backup: $DEVICE_BACKUP"; fi
    die "APK installation failed. Read the ADB message above"
  fi

  if [ -s "$PLAN_ROOT/external.tsv" ]; then
    echo "==> Publishing the external game data"
    while IFS=$'\t' read -r name content leaf first total; do
      [ -n "$name" ] || continue
      case "$name" in
        obb) parent="/sdcard/Android/obb"; final="$REMOTE_OBB_ROOT" ;;
        assetpacks) parent="$REMOTE_FILES_ROOT"; final="$REMOTE_FILES_ROOT/assetpacks" ;;
        *) die "unexpected external data entry: $name" ;;
      esac
      publish_external_tree "$name" "$(local_path "$content")" "$leaf" "$parent" "$final" "$first" "$total"
    done < "$PLAN_ROOT/external.tsv"
  fi

  echo "==> Verifying the installed game and leaving it stopped"
  verify_installed_apks
  if [ "$mode" = "match" ] && [ -n "${before[3]-}" ] && [ "${INSTALLED_AFTER[3]-}" != "${before[3]}" ]; then
    echo "WARNING: the first-install time changed during the update; Android may have reset the app data."
  fi
  stop_game_and_verify
  INSTALL_ACTIVE=0
  start_gpu_boost_helper

  echo ""
  echo "INSTALLED AND VERIFIED on $SERIAL."
  echo "The game is stopped. This installer never starts it; start it from the headset when ready."
  echo "Build manifest: $BUILD_ROOT/build-manifest.json"
  [ -z "$DEVICE_BACKUP" ] || echo "Backup of the previous game files: $DEVICE_BACKUP"
  print_first_launch_hints
  echo "Diagnostic log: $LOG_PATH"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  main "$@"
fi
