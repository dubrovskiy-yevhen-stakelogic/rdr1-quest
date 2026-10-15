#!/usr/bin/env bash
# Red Dead Redemption VR - Update and Install (Linux / macOS).
#
# Brings this folder to the latest version of the mod from GitHub and then runs
# BUILD_AND_INSTALL.sh. Works in a "git clone" folder and in a folder unpacked
# from a GitHub ZIP (the first run turns it into a Git copy). Untracked files
# (exported APKs, build output) are never touched; changes to the mod's own
# files are replaced by the official version.
set -euo pipefail

REPO_URL="https://github.com/dubrovskiy-yevhen-stakelogic/rdr1-quest.git"
BRANCH="main"
REPO_URL="${RDRVR_REPO_URL:-$REPO_URL}"

# Run from a temporary copy: the update rewrites this file.
if [[ -z "${RDRVR_UPDATE_DIR:-}" ]]; then
  export RDRVR_UPDATE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  copy="$(mktemp "${TMPDIR:-/tmp}/rdrvr-update.XXXXXX")"
  cp "${BASH_SOURCE[0]}" "$copy"
  status=0
  bash "$copy" "$@" || status=$?
  rm -f "$copy"
  exit "$status"
fi
cd "$RDRVR_UPDATE_DIR"

echo
echo "============================================================"
echo "  RED DEAD REDEMPTION VR - UPDATE AND INSTALL"
echo "============================================================"
echo

if ! command -v git >/dev/null 2>&1; then
  echo "Git is not installed. Install it and run this script again:"
  if [[ "$(uname -s)" == "Darwin" ]]; then
    echo "  xcode-select --install      (or: brew install git)"
  else
    echo "  sudo apt install git        (Debian/Ubuntu)"
    echo "  sudo dnf install git        (Fedora)"
    echo "  sudo pacman -S git          (Arch)"
  fi
  exit 1
fi
echo "[1/3] Git found."

if [[ ! -d .git ]]; then
  echo "[2/3] First run in this folder: connecting it to the official repository..."
  git init -q
  git remote add origin "$REPO_URL"
elif ! git remote get-url origin >/dev/null 2>&1; then
  git remote add origin "$REPO_URL"
fi
echo "[2/3] Downloading the latest version from $(git remote get-url origin) ..."
old="$(git rev-parse --verify -q HEAD 2>/dev/null || true)"
git -c core.autocrlf=false fetch --depth 1 origin "$BRANCH"
git -c core.autocrlf=false reset -q --hard FETCH_HEAD
git branch -q -f "$BRANCH" FETCH_HEAD >/dev/null 2>&1 || true
git checkout -q "$BRANCH" >/dev/null 2>&1 || true
new="$(git rev-parse --short HEAD)"
if [[ "${old:0:7}" == "$new" ]]; then echo "      Already up to date ($new)."; else echo "      Updated to $new."; fi

if [[ -n "${RDRVR_UPDATE_NO_BUILD:-}" ]]; then echo "[3/3] Skipped: RDRVR_UPDATE_NO_BUILD is set."; exit 0; fi
echo "[3/3] Starting the build and install wizard..."
echo
exec bash ./BUILD_AND_INSTALL.sh "$@"
