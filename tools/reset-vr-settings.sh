#!/usr/bin/env bash
# Removes the VR settings files of Red Dead Redemption VR from a Quest after a
# verified backup. The game is force-stopped first and is never launched.
set -euo pipefail

PACKAGE_NAME="com.rockstargames.rdr"
REMOTE_SETTINGS_ROOT="/sdcard/Android/data/${PACKAGE_NAME}/files/rdrvr"
WORK_DIR="${HOME}/RDRVRBuild"
ADB_ARG=""
ANDROID_SDK_ARG=""
SERIAL=""
ASSUME_YES=0
NON_INTERACTIVE=0
INCLUDE_TELEPORT_SPOTS=0
IS_MSYS=0
ADB=""
ADB_BASE=()
SETTINGS_FILES=(
  controls.cfg
  controls.cfg.tmp
  graphics.cfg
  graphics.cfg.tmp
  vr.cfg
  vr.cfg.tmp
  weapons.cfg
  weapons.cfg.tmp
)
TELEPORT_SPOTS_FILE="teleport-spots.txt"

case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) IS_MSYS=1 ;; esac

usage() {
  cat <<'EOF'
Usage: tools/reset-vr-settings.sh [options]

  --adb PATH                  ADB executable
  --android-sdk DIR           Android SDK root containing platform-tools/adb
  --work-dir DIR              Builder workspace (default: ~/RDRVRBuild)
  --serial SERIAL             Quest serial when more than one device is connected
  --include-teleport-spots    Also remove the saved teleport spots
  --yes                       Remove the listed files without the RESET prompt
  --non-interactive           Never prompt; requires --yes
  -h, --help                  Show this help

Only the VR settings files (controls.cfg, graphics.cfg, vr.cfg, weapons.cfg and their .tmp
files) are removed. They are copied to <work-dir>/settings-backups first. Saved
games, reports, game data and the installed APKs are kept. The game is
force-stopped and is never launched by this tool.
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

remote_file_exists() {
  adb_cmd shell test -f "$1" >/dev/null 2>&1
}

main() {
  local names name path package_output pid_output answer backup_root local_file remote_hash local_hash removed
  local present=()
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --adb|-Adb) ADB_ARG="${2:?missing ADB path}"; shift 2 ;;
      --android-sdk|-AndroidSdk) ANDROID_SDK_ARG="${2:?missing Android SDK path}"; shift 2 ;;
      --work-dir|-WorkDir) WORK_DIR="${2:?missing work directory}"; shift 2 ;;
      --serial|-Serial) SERIAL="${2:?missing Quest serial}"; shift 2 ;;
      --include-teleport-spots|-IncludeTeleportSpots) INCLUDE_TELEPORT_SPOTS=1; shift ;;
      --yes|-Yes) ASSUME_YES=1; shift ;;
      --non-interactive|-NonInteractive) NON_INTERACTIVE=1; shift ;;
      -h|--help) usage; exit 0 ;;
      *) die "unknown option: $1" ;;
    esac
  done
  [ "$NON_INTERACTIVE" -eq 0 ] || [ "$ASSUME_YES" -eq 1 ] ||
    die "--non-interactive requires --yes before settings can be removed"
  [ "$WORK_DIR" != "/" ] || die "--work-dir must be a dedicated directory"

  ADB="$(resolve_adb)"
  echo "ADB: $ADB"
  select_quest
  package_output="$(adb_cmd shell pm path "$PACKAGE_NAME" 2>&1 | tr -d '\r' || true)"
  printf '%s\n' "$package_output" | grep -q '^package:' ||
    die "$PACKAGE_NAME is not installed on the selected Quest"

  names=("${SETTINGS_FILES[@]}")
  if [ "$INCLUDE_TELEPORT_SPOTS" -eq 1 ]; then names+=("$TELEPORT_SPOTS_FILE"); fi
  echo
  echo "VR settings files in the headset:"
  for name in "${names[@]}"; do
    if remote_file_exists "$REMOTE_SETTINGS_ROOT/$name"; then
      present+=("$name")
      echo "  $REMOTE_SETTINGS_ROOT/$name (present)"
    else
      echo "  $REMOTE_SETTINGS_ROOT/$name (absent)"
    fi
  done
  echo
  if [ "$INCLUDE_TELEPORT_SPOTS" -eq 0 ]; then
    echo "Saved teleport spots ($TELEPORT_SPOTS_FILE) are kept; add --include-teleport-spots to remove them too."
  fi
  echo "Saved games, reports, game data and the installed APKs are kept. The settings folder itself is kept."
  if [ "${#present[@]}" -eq 0 ]; then
    echo "No VR settings files are present. Nothing was changed; defaults already apply."
    exit 0
  fi

  if [ "$ASSUME_YES" -ne 1 ]; then
    echo "The game is closed first, the files are copied to this computer, then removed from the headset."
    read -r -p "Type RESET to continue: " answer
    [ "$answer" = "RESET" ] || die "reset was cancelled; no settings were removed"
  fi

  adb_cmd shell am force-stop "$PACKAGE_NAME" >/dev/null || die "could not force-stop the game"
  pid_output="$(adb_cmd shell pidof "$PACKAGE_NAME" 2>/dev/null | tr -d '\r' || true)"
  [ -z "$pid_output" ] || die "$PACKAGE_NAME is still running after force-stop: $pid_output"

  # The game saves these files while it runs, so the backup is taken after the stop.
  backup_root="$WORK_DIR/settings-backups/$(date +%Y%m%d-%H%M%S)"
  mkdir -p "$backup_root"
  : > "$backup_root/SHA256SUMS"
  for name in "${present[@]}"; do
    path="$REMOTE_SETTINGS_ROOT/$name"
    local_file="$backup_root/$name"
    adb_cmd pull "$path" "$(host_path "$local_file")" >/dev/null || die "could not back up $path"
    [ -f "$local_file" ] || die "could not back up $path"
    remote_hash="$(adb_cmd shell toybox sha256sum "$path" | tr -d '\r' | awk '{print tolower($1); exit}')" ||
      die "could not hash $path on the headset"
    local_hash="$(sha256_file "$local_file")"
    [ "$remote_hash" = "$local_hash" ] || die "backup hash mismatch for $name; nothing was removed"
    printf '%s  %s\n' "$local_hash" "$name" >> "$backup_root/SHA256SUMS"
  done
  echo "Backup verified: $backup_root"

  removed=0
  for name in "${present[@]}"; do
    path="$REMOTE_SETTINGS_ROOT/$name"
    adb_cmd shell rm -f -- "$path" >/dev/null || die "could not remove $path"
    removed=$((removed + 1))
  done
  for name in "${names[@]}"; do
    path="$REMOTE_SETTINGS_ROOT/$name"
    adb_cmd shell test ! -e "$path" >/dev/null 2>&1 ||
      die "reset verification failed; the file still exists: $path"
  done

  echo
  echo "VR settings reset verified: $removed file(s) removed. Defaults apply at the next start."
  echo "Backup: $backup_root"
  echo "The game was not launched."
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  main "$@"
fi
