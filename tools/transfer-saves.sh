#!/usr/bin/env bash
# Copies saved games between the PC version of Red Dead Redemption and the Quest.
# The game keeps its saves in private storage that ADB cannot read. At every start the mod
# copies them to rdrvr/save-export and imports the files staged in rdrvr/save-import, after
# backing up each save it replaces in rdrvr/save-backup. The game is never launched.
set -euo pipefail

PACKAGE_NAME="com.rockstargames.rdr"
REMOTE_ROOT="/sdcard/Android/data/${PACKAGE_NAME}/files/rdrvr"
REMOTE_LIST="$REMOTE_ROOT/save-files.txt"
REMOTE_LOG="$REMOTE_ROOT/save-transfer.log"
REMOTE_EXPORT="$REMOTE_ROOT/save-export"
REMOTE_IMPORT="$REMOTE_ROOT/save-import"
REMOTE_BACKUP="$REMOTE_ROOT/save-backup"
REMOTE_TEMP="/data/local/tmp"
SAVE_NAME_REGEX='^(RDR2SAVE[0-9]|RDR2ZOMBIESAVE[0-9]|REDDEAD2HCOR[0-9]|RDR2EXTRAS)\.SAV$'
MIN_SAVE_BYTES=1024
MAX_SAVE_BYTES=1048576
SAVE_MARKER="METAXTR"
SAVE_MARKER_OFFSET=32
WORK_DIR="${HOME}/RDRVRBuild"
ADB_ARG=""
ANDROID_SDK_ARG=""
SERIAL=""
MODE="to-headset"
FROM_DIR=""
TO_DIR=""
INCLUDE_BACKUPS=0
SKIP_EXTRAS=0
ASSUME_YES=0
NON_INTERACTIVE=0
IS_MSYS=0
ADB=""
ADB_BASE=()
STAGED=()
COPIED=()
LIST_EXISTS=0
LISTED_AT=""
ENTRY_REL=()
ENTRY_NAME=()
ENTRY_SIZE=()
ENTRY_MTIME=()

case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) IS_MSYS=1 ;; esac

usage() {
  cat <<'EOF'
Usage: tools/transfer-saves.sh [options] [FOLDER]

  --from DIR                  Send the saves in DIR to the headset (also a bare FOLDER)
  --skip-extras               Do not send RDR2EXTRAS.SAV
  --from-headset              Copy the headset saves to this computer
  --to DIR                    Folder for --from-headset (default: <work-dir>/saves-from-headset/<time>)
  --include-backups           With --from-headset, also copy the backups kept on the headset
  --status                    Show the headset saves, pending imports and the transfer log
  --adb PATH                  ADB executable
  --android-sdk DIR           Android SDK root containing platform-tools/adb
  --work-dir DIR              Builder workspace (default: ~/RDRVRBuild)
  --serial SERIAL             Quest serial when more than one device is connected
  --yes                       Send the saves without the IMPORT prompt
  --non-interactive           Never prompt; sending saves requires --yes
  -h, --help                  Show this help

Sending saves copies RDR2SAVE0-9.SAV, RDR2ZOMBIESAVE0-9.SAV, REDDEAD2HCOR0-9.SAV and
RDR2EXTRAS.SAV of the PC game to the headset. The PC game keeps them in
Documents/Rockstar Games/Red Dead Redemption/Profiles/<profile>. The game is closed
first and the saves are imported the next time it starts; every headset save they
replace is backed up first in rdrvr/save-backup on the headset, and a copy of the
current headset saves is kept in <work-dir>/save-backups. After the import use Load
Game to pick the slot: Continue loads the save with the newest in-game save date.
The game is never launched by this tool.
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
# reach ADB unchanged, so the conversion is switched off per call. ADB never
# reads standard input here, so loops that read lines keep their input.
adb_global() {
  if [ "$IS_MSYS" -eq 1 ]; then MSYS_NO_PATHCONV=1 "$ADB" "$@" </dev/null; else "$ADB" "$@" </dev/null; fi
}

adb_cmd() {
  [ "${#ADB_BASE[@]}" -gt 0 ] || die "no Quest has been selected"
  if [ "$IS_MSYS" -eq 1 ]; then MSYS_NO_PATHCONV=1 "${ADB_BASE[@]}" "$@" </dev/null; else "${ADB_BASE[@]}" "$@" </dev/null; fi
}

sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print tolower($1)}'
  else
    shasum -a 256 "$1" | awk '{print tolower($1)}'
  fi
}

remote_sha256() {
  adb_cmd shell toybox sha256sum "$1" | tr -d '\r' | awk '{print tolower($1); exit}'
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

connect_quest() {
  local package_output
  ADB="$(resolve_adb)"
  echo "ADB: $ADB"
  select_quest
  package_output="$(adb_cmd shell pm path "$PACKAGE_NAME" 2>&1 | tr -d '\r' || true)"
  printf '%s\n' "$package_output" | grep -q '^package:' ||
    die "$PACKAGE_NAME is not installed on the selected Quest"
}

game_running() {
  local pid_output
  pid_output="$(adb_cmd shell pidof "$PACKAGE_NAME" 2>/dev/null | tr -d '\r' || true)"
  [ -n "$pid_output" ]
}

stop_game() {
  local pid_output
  adb_cmd shell am force-stop "$PACKAGE_NAME" >/dev/null || die "could not force-stop the game"
  pid_output="$(adb_cmd shell pidof "$PACKAGE_NAME" 2>/dev/null | tr -d '\r' || true)"
  [ -z "$pid_output" ] || die "$PACKAGE_NAME is still running after force-stop: $pid_output"
  echo "The game is closed."
}

remote_names() {
  adb_cmd shell test -d "$1" >/dev/null 2>&1 || return 0
  adb_cmd shell ls -1 "$1" | tr -d '\r' | sed '/^[[:space:]]*$/d' | LC_ALL=C sort
}

to_upper() {
  printf '%s' "$1" | tr '[:lower:]' '[:upper:]'
}

strip_quotes() {
  local value="$1"
  value="${value#\"}"; value="${value%\"}"
  value="${value#\'}"; value="${value%\'}"
  printf '%s' "$value"
}

format_unix() {
  local seconds="${1:-}" format="${2:-+%Y-%m-%d %H:%M}"
  case "$seconds" in ''|*[!0-9]*) echo "unknown time"; return ;; esac
  date -d "@$seconds" "$format" 2>/dev/null || date -r "$seconds" "$format" 2>/dev/null || echo "$seconds"
}

save_kind() {
  case "$(to_upper "$1")" in
    RDR2ZOMBIESAVE*) echo "Undead Nightmare" ;;
    REDDEAD2HCOR*) echo "hardcore" ;;
    RDR2EXTRAS*) echo "extras" ;;
    RDR2SAVE*) echo "story" ;;
    *) echo "other" ;;
  esac
}

file_size() {
  wc -c < "$1" | tr -d ' '
}

save_problem() {
  local file="$1" size marker
  size="$(file_size "$file")"
  if [ "$size" -le "$MIN_SAVE_BYTES" ]; then echo "too small ($size bytes)"; return; fi
  if [ "$size" -ge "$MAX_SAVE_BYTES" ]; then echo "too large ($size bytes)"; return; fi
  marker="$(dd if="$file" bs=1 skip="$SAVE_MARKER_OFFSET" count="${#SAVE_MARKER}" 2>/dev/null | LC_ALL=C tr -d '\000')"
  [ "$marker" = "$SAVE_MARKER" ] || echo "no Red Dead Redemption save header"
}

# save-files.txt is written at every game start: one line per save with its private path,
# size and modification time (Unix seconds), separated by tabs, after a '#' header line.
read_headset_list() {
  local text line rel size mtime rest
  LIST_EXISTS=0; LISTED_AT=""; ENTRY_REL=(); ENTRY_NAME=(); ENTRY_SIZE=(); ENTRY_MTIME=()
  adb_cmd shell test -f "$REMOTE_LIST" >/dev/null 2>&1 || return 0
  text="$(adb_cmd shell cat "$REMOTE_LIST" | tr -d '\r')" || die "could not read $REMOTE_LIST"
  LIST_EXISTS=1
  while IFS= read -r line; do
    [ -n "$line" ] || continue
    case "$line" in
      '#'*)
        LISTED_AT="$(printf '%s\n' "$line" | tr ' ' '\n' | grep -E '^[0-9]{9,12}$' | sed -n 1p || true)"
        continue
        ;;
    esac
    IFS=$'\t' read -r rel size mtime rest <<< "$line"
    [ -n "$rel" ] && [ -n "$mtime" ] || continue
    ENTRY_REL+=("$rel"); ENTRY_NAME+=("${rel##*/}"); ENTRY_SIZE+=("$size"); ENTRY_MTIME+=("$mtime")
  done <<< "$text"
}

print_headset_list() {
  local i
  for ((i = 0; i < ${#ENTRY_REL[@]}; i++)); do
    printf '  %-20s %-16s %8s bytes  saved %s\n' "${ENTRY_NAME[$i]}" "$(save_kind "${ENTRY_NAME[$i]}")" \
      "${ENTRY_SIZE[$i]}" "$(format_unix "${ENTRY_MTIME[$i]}")"
  done
}

# Copies every .SAV file of a headset folder to a local folder as NAME.SAV and verifies each
# copy. save-export and save-backup name each file by its private path with '/' -> '__'.
copy_headset_saves() {
  local remote_dir="$1" local_dir="$2" names name local_name remote local_file remote_hash local_hash
  COPIED=()
  names="$(remote_names "$remote_dir")" || die "could not list $remote_dir on the headset"
  while IFS= read -r name; do
    [ -n "$name" ] || continue
    case "$(to_upper "$name")" in *.SAV) ;; *) continue ;; esac
    mkdir -p "$local_dir"
    local_name="${name##*__}"
    if [ -e "$local_dir/$local_name" ]; then local_name="$name"; fi
    remote="$remote_dir/$name"
    local_file="$local_dir/$local_name"
    adb_cmd pull "$remote" "$(host_path "$local_file")" >/dev/null || die "could not copy $remote"
    [ -f "$local_file" ] || die "could not copy $remote"
    remote_hash="$(remote_sha256 "$remote")" || die "could not hash $remote on the headset"
    local_hash="$(sha256_file "$local_file")"
    [ "$remote_hash" = "$local_hash" ] || die "the copy of $remote does not match the headset file"
    printf '%s  %s\n' "$local_hash" "$local_name" >> "$local_dir/SHA256SUMS"
    COPIED+=("$local_name")
  done <<< "$names"
}

# The PC game keeps one folder per profile in Documents/Rockstar Games/Red Dead Redemption/Profiles.
# It is found automatically only under Git Bash on Windows; elsewhere the folder is asked for.
find_pc_profile() {
  local root file upper newest=""
  [ "$IS_MSYS" -eq 1 ] && [ -n "${USERPROFILE:-}" ] && command -v cygpath >/dev/null 2>&1 || return 1
  root="$(cygpath -u "$USERPROFILE")/Documents/Rockstar Games/Red Dead Redemption/Profiles"
  for file in "$root"/*/*; do
    [ -f "$file" ] || continue
    upper="$(to_upper "${file##*/}")"
    [[ "$upper" =~ $SAVE_NAME_REGEX ]] || continue
    if [ -z "$newest" ] || [ "$file" -nt "$newest" ]; then newest="$file"; fi
  done
  [ -n "$newest" ] || return 1
  dirname "$newest"
}

cleanup_staged() {
  local status=$? path
  if [ "$status" -ne 0 ] && [ "${#STAGED[@]}" -gt 0 ]; then
    for path in "${STAGED[@]}"; do adb_cmd shell rm -f -- "$path" >/dev/null 2>&1 || true; done
    echo "The files staged by this run were removed from the headset; its saves are unchanged." >&2
  fi
}

to_headset() {
  local file name upper problem i j match stage temp target remote_hash local_hash answer
  local new_folder="" first_rel pending_text staged_text found
  local candidates=() selected=() ignored=() problems=()
  local plan_file=() plan_name=() plan_stage=() plan_match=() obsolete=() replaced=()
  local backup_root

  # 1. The saves on this computer.
  if [ -z "$FROM_DIR" ]; then
    FROM_DIR="$(find_pc_profile)" || FROM_DIR=""
    if [ -n "$FROM_DIR" ]; then
      echo "PC profile folder with the newest saves: $FROM_DIR"
      echo "Pass --from to use another folder."
    elif [ "$NON_INTERACTIVE" -eq 1 ]; then
      die "pass --from with the folder that holds the .SAV files of the PC game"
    else
      echo "The PC game keeps its saves in Documents/Rockstar Games/Red Dead Redemption/Profiles/<profile>."
      read -r -p "Folder with the PC saves: " FROM_DIR
      FROM_DIR="$(strip_quotes "$FROM_DIR")"
      [ -n "$FROM_DIR" ] || die "no folder was given"
    fi
  fi
  if [ -f "$FROM_DIR" ]; then
    candidates=("$FROM_DIR")
  elif [ -d "$FROM_DIR" ]; then
    for file in "$FROM_DIR"/*; do
      if [ -f "$file" ]; then candidates+=("$file"); fi
    done
  else
    die "--from was not found: $FROM_DIR"
  fi
  echo "Source: $FROM_DIR"

  for file in ${candidates[@]+"${candidates[@]}"}; do
    name="${file##*/}"
    upper="$(to_upper "$name")"
    if ! [[ "$upper" =~ $SAVE_NAME_REGEX ]]; then
      case "$upper" in *.SAV) ignored+=("$name (not a save slot name)") ;; esac
      continue
    fi
    if [ "$SKIP_EXTRAS" -eq 1 ] && [ "$upper" = "RDR2EXTRAS.SAV" ]; then
      ignored+=("$name (--skip-extras)")
      continue
    fi
    selected+=("$file")
  done
  [ "${#selected[@]}" -gt 0 ] ||
    die "no save files (RDR2SAVE0.SAV, RDR2ZOMBIESAVE0.SAV, REDDEAD2HCOR0.SAV, RDR2EXTRAS.SAV and so on) were found in $FROM_DIR"
  for file in "${selected[@]}"; do
    problem="$(save_problem "$file")"
    if [ -n "$problem" ]; then problems+=("  ${file##*/}: $problem"); fi
  done
  if [ "${#problems[@]}" -gt 0 ]; then
    echo "These files are not Red Dead Redemption saves; nothing was sent to the headset:" >&2
    printf '%s\n' "${problems[@]}" >&2
    die "invalid save files"
  fi
  echo
  echo "Saves to send:"
  for file in "${selected[@]}"; do
    upper="$(to_upper "${file##*/}")"
    printf '  %-20s %-16s %8s bytes\n' "$upper" "$(save_kind "$upper")" "$(file_size "$file")"
  done
  for name in ${ignored[@]+"${ignored[@]}"}; do echo "  ignored: $name"; done
  for file in "${selected[@]}"; do
    upper="$(to_upper "${file##*/}")"
    case "$upper" in
      *[4-9].SAV) echo "Note: the Android game has save slots 0 to 3; $upper may not appear in Load Game." ;;
    esac
  done

  # 2. The saves on the headset, as listed by the mod at the last game start.
  echo
  connect_quest
  read_headset_list
  [ "$LIST_EXISTS" -eq 1 ] ||
    die "the mod has not listed the headset saves yet (rdrvr/save-files.txt is missing). Start the game once until its main menu, close it, then run TRANSFER_SAVES again. If the file is still missing, build and install the mod from this kit again"
  [ "${#ENTRY_REL[@]}" -gt 0 ] ||
    die "the game has no saves on the headset yet, so the folder for imported saves is unknown. Start a new game on the headset, save it once, close the game, then run TRANSFER_SAVES again"
  first_rel="$(printf '%s\n' "${ENTRY_REL[@]}" | LC_ALL=C sort | sed -n 1p)"
  case "$first_rel" in */*) new_folder="${first_rel%/*}/" ;; esac

  # A save that exists is staged under its private path so the mod replaces exactly that file;
  # a new one is staged under its plain name and created next to the existing saves.
  for file in "${selected[@]}"; do
    upper="$(to_upper "${file##*/}")"
    match=-1
    for ((i = 0; i < ${#ENTRY_NAME[@]}; i++)); do
      if [ "$(to_upper "${ENTRY_NAME[$i]}")" = "$upper" ]; then match=$i; break; fi
    done
    plan_file+=("$file"); plan_name+=("$upper"); plan_match+=("$match")
    if [ "$match" -ge 0 ]; then
      stage="${ENTRY_REL[$match]}"
      plan_stage+=("${stage//\//__}")
      replaced+=("$match")
    else
      plan_stage+=("$upper")
    fi
  done
  pending_text="$(remote_names "$REMOTE_IMPORT")" || die "could not list $REMOTE_IMPORT on the headset"
  while IFS= read -r name; do
    [ -n "$name" ] || continue
    case "$(to_upper "$name")" in *.SAV) ;; *) continue ;; esac
    found=0
    for stage in "${plan_stage[@]}"; do
      if [ "$stage" = "$name" ]; then found=1; break; fi
    done
    if [ "$found" -eq 0 ]; then obsolete+=("$name"); fi
  done <<< "$pending_text"

  echo
  echo "Saves on the headset when the game last started ($(format_unix "$LISTED_AT")). Saves imported or made since then are not listed:"
  print_headset_list
  echo
  echo "Changes at the next game start:"
  for ((i = 0; i < ${#plan_name[@]}; i++)); do
    match="${plan_match[$i]}"
    if [ "$match" -ge 0 ]; then
      echo "  REPLACE  ${plan_name[$i]} ($(save_kind "${plan_name[$i]}")); the headset save from $(format_unix "${ENTRY_MTIME[$match]}") is backed up first"
    else
      echo "  NEW      ${plan_name[$i]} ($(save_kind "${plan_name[$i]}")); not listed at the last start, imported as ${new_folder}${plan_name[$i]}"
    fi
  done
  for ((i = 0; i < ${#ENTRY_REL[@]}; i++)); do
    found=0
    for j in ${replaced[@]+"${replaced[@]}"}; do
      if [ "$j" -eq "$i" ]; then found=1; break; fi
    done
    if [ "$found" -eq 0 ]; then echo "  keep     ${ENTRY_NAME[$i]}"; fi
  done
  for name in ${obsolete[@]+"${obsolete[@]}"}; do
    echo "  An earlier transfer staged $name and it was not imported yet; it is removed so that only this transfer is applied."
  done
  echo
  echo "Every headset save that is replaced is backed up first on the headset in $REMOTE_BACKUP."
  echo "A copy of the current headset saves is also kept on this computer."

  if [ "$ASSUME_YES" -ne 1 ]; then
    echo "The game is closed first; progress that was not saved in a running game is lost."
    read -r -p "Type IMPORT to continue: " answer
    [ "$answer" = "IMPORT" ] || die "the transfer was cancelled; nothing was sent to the headset"
  fi

  # 3. Close the game, keep a local copy of the headset saves, stage the new ones.
  stop_game
  backup_root="$WORK_DIR/save-backups/$(date +%Y%m%d-%H%M%S)"
  copy_headset_saves "$REMOTE_EXPORT" "$backup_root"
  if [ "${#COPIED[@]}" -gt 0 ]; then
    echo "Copy of the headset saves on this computer (as of the last game start): $backup_root"
  fi
  for ((i = 0; i < ${#ENTRY_NAME[@]}; i++)); do
    found=0
    for name in ${COPIED[@]+"${COPIED[@]}"}; do
      if [ "$(to_upper "$name")" = "$(to_upper "${ENTRY_NAME[$i]}")" ]; then found=1; break; fi
    done
    if [ "$found" -eq 0 ]; then
      echo "WARNING: the headset has no exported copy of ${ENTRY_NAME[$i]}; only the backup on the headset will hold it."
    fi
  done

  adb_cmd shell mkdir -p "$REMOTE_IMPORT" >/dev/null || die "could not create $REMOTE_IMPORT"
  adb_cmd shell chmod 777 "$REMOTE_IMPORT" >/dev/null 2>&1 ||
    echo "WARNING: could not open $REMOTE_IMPORT to the game."
  for name in ${obsolete[@]+"${obsolete[@]}"}; do
    adb_cmd shell rm -f -- "$REMOTE_IMPORT/$name" >/dev/null || die "could not remove the earlier staged file $name"
  done
  for ((i = 0; i < ${#plan_stage[@]}; i++)); do
    stage="${plan_stage[$i]}"
    temp="$REMOTE_TEMP/rdrvr-$stage"
    target="$REMOTE_IMPORT/$stage"
    STAGED+=("$temp" "$target")
    adb_cmd push "$(host_path "${plan_file[$i]}")" "$temp" >/dev/null || die "could not copy ${plan_name[$i]} to the headset"
    adb_cmd shell cp "$temp" "$target" >/dev/null || die "could not stage $target"
    adb_cmd shell chmod 666 "$target" >/dev/null 2>&1 || true
    adb_cmd shell rm -f -- "$temp" >/dev/null 2>&1 || true
    remote_hash="$(remote_sha256 "$target")" || die "could not hash $target on the headset"
    local_hash="$(sha256_file "${plan_file[$i]}")"
    [ "$remote_hash" = "$local_hash" ] || die "the staged copy of ${plan_name[$i]} does not match the local file"
  done
  staged_text="$(remote_names "$REMOTE_IMPORT")" || die "could not list $REMOTE_IMPORT on the headset"
  for stage in "${plan_stage[@]}"; do
    printf '%s\n' "$staged_text" | grep -qxF -- "$stage" ||
      die "staging verification failed; $stage is missing in $REMOTE_IMPORT"
  done
  STAGED=()

  echo
  echo "${#plan_stage[@]} save file(s) are staged and verified. The headset saves change only when the game starts."
  for ((i = 0; i < ${#plan_name[@]}; i++)); do
    if [ "${plan_match[$i]}" -ge 0 ]; then echo "  ${plan_name[$i]} (replaces)"; else echo "  ${plan_name[$i]} (new)"; fi
  done
  echo
  echo "Next steps"
  echo "  1. Start the game on the headset once. The saves are imported while it starts; each replaced"
  echo "     save is backed up first in $REMOTE_BACKUP/<start time>."
  echo "  2. In the main menu use Load Game and pick the slot. Continue loads the save with the newest"
  echo "     in-game save date, which may be a different slot."
  echo "  3. TRANSFER_SAVES --status shows the import log after that start."
  if [ "${#COPIED[@]}" -gt 0 ]; then echo "Local copy of the previous headset saves: $backup_root"; fi
  echo "The game was not launched."
}

from_headset() {
  local destination names name folder folders label local_dir answer copy_backups=0 profile count=0 problem
  if [ -n "$TO_DIR" ]; then
    destination="$TO_DIR"
  else
    destination="$WORK_DIR/saves-from-headset/$(date +%Y%m%d-%H%M%S)"
  fi
  connect_quest
  read_headset_list
  names="$(remote_names "$REMOTE_EXPORT")" || die "could not list $REMOTE_EXPORT on the headset"
  while IFS= read -r name; do
    [ -n "$name" ] || continue
    case "$(to_upper "$name")" in *.SAV) ;; *) continue ;; esac
    count=$((count + 1))
    [ ! -e "$destination/${name##*__}" ] ||
      die "$destination/${name##*__} already exists. Choose an empty folder with --to; existing saves are never overwritten"
  done <<< "$names"
  [ "$count" -gt 0 ] ||
    die "the headset has no exported saves yet (rdrvr/save-export is empty). Start the game once until its main menu, close it, then run TRANSFER_SAVES --from-headset again"

  copy_headset_saves "$REMOTE_EXPORT" "$destination"
  echo
  echo "Headset saves as of the last game start ($(format_unix "$LISTED_AT")):"
  for name in ${COPIED[@]+"${COPIED[@]}"}; do
    printf '  %-20s %-16s %8s bytes\n' "$name" "$(save_kind "$name")" "$(file_size "$destination/$name")"
    problem="$(save_problem "$destination/$name")"
    if [ -n "$problem" ]; then echo "    WARNING: $problem"; fi
  done
  echo "Copied to: $destination"
  echo "The mod copies the saves out when the game starts, before it imports staged saves. Saves made or"
  echo "imported after that are copied at the next start: start the game once until its main menu, close"
  echo "it, and run this again to include them."

  folders="$(remote_names "$REMOTE_BACKUP")" || die "could not list $REMOTE_BACKUP on the headset"
  if [ -n "$folders" ]; then
    echo
    echo "The headset also keeps $(printf '%s\n' "$folders" | wc -l | tr -d ' ') backup folder(s) of saves that imports replaced ($REMOTE_BACKUP)."
    copy_backups="$INCLUDE_BACKUPS"
    if [ "$copy_backups" -eq 0 ] && [ "$NON_INTERACTIVE" -eq 0 ]; then
      read -r -p "Type Y to copy them too, or press Enter to skip: " answer
      case "$answer" in y|Y|yes|YES|Yes) copy_backups=1 ;; esac
    fi
    if [ "$copy_backups" -eq 1 ]; then
      while IFS= read -r folder; do
        [ -n "$folder" ] || continue
        label="$folder"
        case "$folder" in *[!0-9]*) ;; *) label="$(format_unix "$folder" '+%Y%m%d-%H%M%S')" ;; esac
        local_dir="$destination/headset-backups/$label"
        copy_headset_saves "$REMOTE_BACKUP/$folder" "$local_dir"
        echo "  backup $label: ${#COPIED[@]} file(s)"
      done <<< "$folders"
      echo "Backups copied to: $destination/headset-backups"
    elif [ "$NON_INTERACTIVE" -eq 1 ]; then
      echo "Add --include-backups to copy them too."
    fi
  fi

  echo
  echo "To play them on the PC: close the PC game, keep a copy of its profile folder, then copy the"
  echo ".SAV files from $destination into the profile folder:"
  profile="$(find_pc_profile)" || profile=""
  if [ -n "$profile" ]; then echo "  $profile"; else echo "  Documents/Rockstar Games/Red Dead Redemption/Profiles/<profile>"; fi
  echo "The game was not launched."
}

show_status() {
  local names name folder folders files label pending="" done_names="" count=0
  connect_quest
  read_headset_list
  echo
  if [ "$LIST_EXISTS" -eq 0 ]; then
    echo "The save list (rdrvr/save-files.txt) is not present yet. The mod writes it when the game starts."
  else
    echo "Saves on the headset when the game last started ($(format_unix "$LISTED_AT")). Saves imported or made since then are not listed:"
    if [ "${#ENTRY_REL[@]}" -eq 0 ]; then echo "  none"; fi
    print_headset_list
  fi

  names="$(remote_names "$REMOTE_IMPORT")" || die "could not list $REMOTE_IMPORT on the headset"
  while IFS= read -r name; do
    [ -n "$name" ] || continue
    case "$(to_upper "$name")" in
      *.SAV) pending="$pending  $name"$'\n' ;;
      *.SAV.DONE) done_names="${done_names:+$done_names, }$name" ;;
    esac
  done <<< "$names"
  echo
  if [ -n "$pending" ]; then
    echo "Waiting for import at the next game start:"
    printf '%s' "$pending"
  else
    echo "No saves are waiting for import."
  fi
  if [ -n "$done_names" ]; then echo "Imported earlier: $done_names"; fi

  names="$(remote_names "$REMOTE_EXPORT")" || die "could not list $REMOTE_EXPORT on the headset"
  while IFS= read -r name; do
    case "$(to_upper "$name")" in *.SAV) count=$((count + 1)) ;; esac
  done <<< "$names"
  echo "Exported copies in rdrvr/save-export: $count"
  folders="$(remote_names "$REMOTE_BACKUP")" || die "could not list $REMOTE_BACKUP on the headset"
  if [ -n "$folders" ]; then
    echo "Backups of replaced saves in rdrvr/save-backup:"
    while IFS= read -r folder; do
      [ -n "$folder" ] || continue
      files="$(remote_names "$REMOTE_BACKUP/$folder" | tr '\n' ' ')" || files=""
      label="$folder"
      case "$folder" in *[!0-9]*) ;; *) label="$(format_unix "$folder")" ;; esac
      echo "  $folder ($label): $files"
    done <<< "$folders"
  else
    echo "No backups of replaced saves yet."
  fi

  echo
  if adb_cmd shell test -f "$REMOTE_LOG" >/dev/null 2>&1; then
    echo "Last lines of rdrvr/save-transfer.log:"
    adb_cmd shell tail -n 20 "$REMOTE_LOG" | tr -d '\r' | sed 's/^/  /' || true
  else
    echo "No save transfer log yet."
  fi
  if game_running; then
    echo
    echo "The game is running. Saves staged now are imported at the next start after it is closed."
  fi
  echo "The game was not launched."
}

set_mode() {
  [ "$MODE" = "to-headset" ] || [ "$MODE" = "$1" ] || die "use either --from-headset or --status, not both"
  MODE="$1"
}

main() {
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --from|-From) FROM_DIR="$(strip_quotes "${2:?missing folder}")"; shift 2 ;;
      --from-headset|-FromHeadset) set_mode "from-headset"; shift ;;
      --to|-To) TO_DIR="$(strip_quotes "${2:?missing folder}")"; shift 2 ;;
      --include-backups|-IncludeBackups) INCLUDE_BACKUPS=1; shift ;;
      --status|-Status) set_mode "status"; shift ;;
      --skip-extras|-SkipExtras) SKIP_EXTRAS=1; shift ;;
      --adb|-Adb) ADB_ARG="${2:?missing ADB path}"; shift 2 ;;
      --android-sdk|-AndroidSdk) ANDROID_SDK_ARG="${2:?missing Android SDK path}"; shift 2 ;;
      --work-dir|-WorkDir) WORK_DIR="${2:?missing work directory}"; shift 2 ;;
      --serial|-Serial) SERIAL="${2:?missing Quest serial}"; shift 2 ;;
      --yes|-Yes) ASSUME_YES=1; shift ;;
      --non-interactive|-NonInteractive) NON_INTERACTIVE=1; shift ;;
      -h|--help) usage; exit 0 ;;
      -*) die "unknown option: $1" ;;
      *)
        [ -z "$FROM_DIR" ] || die "only one source folder can be given: $1"
        FROM_DIR="$1"; shift
        ;;
    esac
  done
  if [ "$MODE" != "to-headset" ]; then
    [ -z "$FROM_DIR" ] || die "--from is used only when sending saves to the headset"
    [ "$SKIP_EXTRAS" -eq 0 ] || die "--skip-extras is used only when sending saves to the headset"
  fi
  if [ "$MODE" != "from-headset" ]; then
    [ -z "$TO_DIR" ] || die "--to is used only with --from-headset"
    [ "$INCLUDE_BACKUPS" -eq 0 ] || die "--include-backups is used only with --from-headset"
  fi
  if [ "$MODE" = "to-headset" ] && [ "$NON_INTERACTIVE" -eq 1 ] && [ "$ASSUME_YES" -eq 0 ]; then
    die "--non-interactive requires --yes before saves can be sent to the headset"
  fi
  [ "$WORK_DIR" != "/" ] || die "--work-dir must be a dedicated directory"

  trap cleanup_staged EXIT
  case "$MODE" in
    status) show_status ;;
    from-headset) from_headset ;;
    *) to_headset ;;
  esac
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  main "$@"
fi
