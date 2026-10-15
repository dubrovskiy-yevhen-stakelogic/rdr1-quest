#!/usr/bin/env bash
# Starts, stops or shows the GPU boost helper (tools/rdrvr_boost.sh) on the Quest.
# The helper raises the GPU clock only while the game runs and asks for it in its
# GRAPHICS menu; a headset restart stops it. The game is never launched.
set -euo pipefail

HELPER_REMOTE="/data/local/tmp/rdrvr_boost.sh"
HELPER_LOCAL="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)/rdrvr_boost.sh"
WORK_DIR="${HOME}/RDRVRBuild"
ADB_ARG=""
ANDROID_SDK_ARG=""
SERIAL=""
ACTION="start"
NON_INTERACTIVE=0
IS_MSYS=0
ADB=""
ADB_BASE=()

case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) IS_MSYS=1 ;; esac

usage() {
  cat <<'EOF'
Usage: tools/gpu-boost.sh [options]

  --off                       Stop the helper; the GPU level it set is cleared
  --status                    Show whether the helper runs and the current GPU level
  --adb PATH                  ADB executable
  --android-sdk DIR           Android SDK root containing platform-tools/adb
  --work-dir DIR              Builder workspace (default: ~/RDRVRBuild)
  --serial SERIAL             Quest serial when more than one device is connected
  --non-interactive           Never prompt
  -h, --help                  Show this help

Without --off or --status the helper is copied to the headset and started.
Run it once after every headset restart. In the VR menu, GRAPHICS > GPU BOOST
chooses the level (OFF, 640 or 690 MHz) and shows NO HELPER when the helper is
not running. The boost applies only while the game runs.
EOF
}

die() {
  echo "ERROR: $*" >&2
  exit 1
}

validate_adb() {
  local candidate="$1" output
  [ -f "$candidate" ] || return 1
  output="$("$candidate" version 2>&1)" || return 1
  printf '%s\n' "$output" | grep -q "Android Debug Bridge"
}

resolve_adb() {
  local candidate sdk host_tag
  if [ -n "$ADB_ARG" ]; then
    validate_adb "$ADB_ARG" || die "--adb is not a working ADB executable: $ADB_ARG"
    printf '%s\n' "$ADB_ARG"
    return
  fi
  case "$(uname -s):$(uname -m)" in
    Linux:x86_64|Linux:amd64) host_tag="linux-x64" ;;
    Darwin:x86_64) host_tag="mac-x64" ;;
    Darwin:arm64|Darwin:aarch64) host_tag="mac-arm64" ;;
    *) host_tag="" ;;
  esac
  if [ -n "$host_tag" ]; then
    candidate="$WORK_DIR/.tools/platform-tools-37.0.1-${host_tag}/platform-tools/adb"
    if validate_adb "$candidate"; then printf '%s\n' "$candidate"; return; fi
  fi
  if [ "$IS_MSYS" -eq 1 ]; then
    candidate="$WORK_DIR/.tools/platform-tools-37.0.1/platform-tools/adb.exe"
    if validate_adb "$candidate"; then printf '%s\n' "$candidate"; return; fi
  fi
  for sdk in "$ANDROID_SDK_ARG" "${ANDROID_SDK_ROOT:-}" "${ANDROID_HOME:-}" "$WORK_DIR/.android-sdk"; do
    [ -n "$sdk" ] || continue
    for candidate in "$sdk/platform-tools/adb" "$sdk/platform-tools/adb.exe"; do
      if validate_adb "$candidate"; then printf '%s\n' "$candidate"; return; fi
    done
  done
  if command -v adb >/dev/null 2>&1; then
    candidate="$(command -v adb)"
    if validate_adb "$candidate"; then printf '%s\n' "$candidate"; return; fi
  fi
  die "ADB was not found. Run BUILD_AND_INSTALL once, install Android Platform-Tools, or pass --adb/--android-sdk/--work-dir."
}

host_path() {
  if [ "$IS_MSYS" -eq 1 ] && command -v cygpath >/dev/null 2>&1; then
    cygpath -w "$1"
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

sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print tolower($1)}'
  else
    shasum -a 256 "$1" | awk '{print tolower($1)}'
  fi
}

select_quest() {
  local output line serial state rest manufacturer model identity choice index deadline
  local ready=()
  adb_global start-server >/dev/null 2>&1 || true
  deadline=$(( $(date +%s) + 60 ))
  while true; do
    output="$(adb_global devices -l 2>&1 | tr -d '\r')" || output=""
    ready=()
    while IFS= read -r line; do
      [ -n "$line" ] || continue
      case "$line" in 'List of devices attached'*|'*'*) continue ;; esac
      read -r serial state rest <<< "$line"
      [ "$state" = "device" ] || continue
      if [ -z "$SERIAL" ] || [ "$serial" = "$SERIAL" ]; then ready+=("$serial"); fi
    done <<< "$output"
    [ "${#ready[@]}" -eq 0 ] || break
    if [ "$(date +%s)" -ge "$deadline" ]; then
      printf '%s\n' "$output" | grep -q "unauthorized" &&
        die "the headset is connected but USB debugging is not allowed yet. Put on the headset and accept Allow USB debugging"
      printf '%s\n' "$output" | grep -q "offline" &&
        die "the headset connection is offline. Reconnect the USB cable and try again"
      printf '%s\n' "$output" | grep -q "no permissions" &&
        die "ADB cannot access the headset. Install the Android udev rules and log in again"
      [ -z "$SERIAL" ] || die "the requested device $SERIAL was not found by ADB"
      die "no Quest headset was found over ADB. Check Developer Mode, the USB-C data cable and the Allow USB debugging prompt"
    fi
    echo "Waiting for the Quest headset: connect it with a USB-C data cable, put it on and accept Allow USB debugging."
    sleep 2
  done
  if [ "${#ready[@]}" -eq 1 ]; then
    SERIAL="${ready[0]}"
  elif [ "$NON_INTERACTIVE" -eq 1 ]; then
    die "more than one ADB device is connected; pass --serial with --non-interactive"
  else
    echo "Connected ADB devices:"
    index=1
    for line in "${ready[@]}"; do echo "  [$index] $line"; index=$((index + 1)); done
    read -r -p "Choose the Quest headset by number: " choice
    case "$choice" in ''|*[!0-9]*) die "invalid device selection" ;; esac
    [ "$choice" -ge 1 ] && [ "$choice" -le "${#ready[@]}" ] || die "invalid device selection"
    SERIAL="${ready[$((choice - 1))]}"
  fi

  ADB_BASE=("$ADB" -s "$SERIAL")
  manufacturer="$(adb_cmd shell getprop ro.product.manufacturer | tr -d '\r')" || die "could not read the device manufacturer"
  model="$(adb_cmd shell getprop ro.product.model | tr -d '\r')" || die "could not read the device model"
  identity="$(printf '%s %s' "$manufacturer" "$model" | tr '[:upper:]' '[:lower:]')"
  case "$identity" in *quest*|*oculus*|*meta*) ;; *) die "the selected device does not identify itself as a Meta Quest: $manufacturer $model" ;; esac
  echo "Selected Quest: $SERIAL ($manufacturer $model)"
}

gpu_level() {
  adb_cmd shell getprop debug.oculus.gpuLevel 2>/dev/null | tr -d '\r' || true
}

helper_status() {
  adb_cmd shell sh "$HELPER_REMOTE" status 2>&1 | tr -d '\r' || true
}

print_guidance() {
  echo
  echo "GPU BOOST"
  echo "  - Run GPU_BOOST once after every headset restart. A restart stops the helper."
  echo "  - In the VR menu, GRAPHICS > GPU BOOST chooses the level: OFF, 640 or 690 MHz (default 690)."
  echo "    It shows NO HELPER when the helper is not running."
  echo "  - The boost applies only while the game runs. Home and other apps keep their normal clocks."
  echo "  - Higher clocks drain the battery faster and warm the headset. When the headset gets hot,"
  echo "    the game shows a red overheat warning and lowers 690 to 640 MHz."
  echo "  - GPU_BOOST --off stops the helper; GPU_BOOST --status shows its state."
}

main() {
  local helper_present=0 status_text="" level stop_output remote_hash local_hash attempt
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --off|-Off) [ "$ACTION" = "start" ] || die "use either --off or --status, not both"; ACTION="off"; shift ;;
      --status|-Status) [ "$ACTION" = "start" ] || die "use either --off or --status, not both"; ACTION="status"; shift ;;
      --adb|-Adb) ADB_ARG="${2:?missing ADB path}"; shift 2 ;;
      --android-sdk|-AndroidSdk) ANDROID_SDK_ARG="${2:?missing Android SDK path}"; shift 2 ;;
      --work-dir|-WorkDir) WORK_DIR="${2:?missing work directory}"; shift 2 ;;
      --serial|-Serial) SERIAL="${2:?missing Quest serial}"; shift 2 ;;
      --non-interactive|-NonInteractive) NON_INTERACTIVE=1; shift ;;
      -h|--help) usage; exit 0 ;;
      *) die "unknown option: $1" ;;
    esac
  done
  [ "$WORK_DIR" != "/" ] || die "--work-dir must be a dedicated directory"
  [ -f "$HELPER_LOCAL" ] || die "the source kit is incomplete. Missing: $HELPER_LOCAL"

  ADB="$(resolve_adb)"
  echo "ADB: $ADB"
  select_quest
  if adb_cmd shell test -f "$HELPER_REMOTE" >/dev/null 2>&1; then helper_present=1; fi

  case "$ACTION" in
    status)
      echo
      if [ "$helper_present" -eq 0 ]; then
        level="$(gpu_level)"
        echo "The GPU boost helper has not been started on this headset. Run GPU_BOOST to start it."
        echo "debug.oculus.gpuLevel: ${level:-not set}"
        exit 0
      fi
      status_text="$(helper_status)"
      printf '%s\n' "$status_text"
      if printf '%s\n' "$status_text" | grep -q '^helper: running'; then
        echo "The GPU boost helper is running."
      else
        echo "The GPU boost helper is not running. Run GPU_BOOST to start it."
      fi
      exit 0
      ;;
    off)
      if [ "$helper_present" -eq 1 ]; then
        if stop_output="$(adb_cmd shell sh "$HELPER_REMOTE" stop 2>&1 | tr -d '\r')"; then
          printf '%s\n' "$stop_output"
        else
          printf '%s\n' "$stop_output"
          die "the GPU boost helper did not stop. Restart the headset to restore the normal GPU clocks"
        fi
      else
        echo "The GPU boost helper has not been started on this headset; nothing to stop."
      fi
      level="$(gpu_level)"
      if [ -n "$level" ]; then
        echo "The GPU level is still $level. It was set by another tool, not by this helper. Restart the headset to restore the normal GPU clocks."
      else
        echo "GPU boost is off. The headset uses its normal GPU clocks."
      fi
      echo "The game was not launched."
      exit 0
      ;;
  esac

  echo "Copying the GPU boost helper to the headset..."
  adb_cmd push "$(host_path "$HELPER_LOCAL")" "$HELPER_REMOTE" >/dev/null || die "could not copy the helper to the headset"
  remote_hash="$(adb_cmd shell toybox sha256sum "$HELPER_REMOTE" | tr -d '\r' | awk '{print tolower($1); exit}')" ||
    die "could not hash the helper on the headset"
  local_hash="$(sha256_file "$HELPER_LOCAL")"
  [ "$remote_hash" = "$local_hash" ] || die "the helper copy on the headset does not match the kit file"
  adb_cmd shell sh "$HELPER_REMOTE" start >/dev/null || die "could not start the helper"
  for attempt in 1 2 3 4 5; do
    sleep 2
    status_text="$(helper_status)"
    if printf '%s\n' "$status_text" | grep -q '^helper: running'; then break; fi
  done
  echo
  printf '%s\n' "$status_text"
  printf '%s\n' "$status_text" | grep -q '^helper: running' ||
    die "the GPU boost helper did not start. Reconnect the headset and run GPU_BOOST again"
  echo
  echo "GPU boost helper is running on the headset."
  print_guidance
  echo
  echo "The game was not launched."
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  main "$@"
fi
