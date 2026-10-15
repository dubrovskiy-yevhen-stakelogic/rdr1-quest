#!/usr/bin/env bash
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
TRANSFER_SCRIPT="$ROOT/tools/transfer-saves.sh"

if [ ! -f "$TRANSFER_SCRIPT" ]; then
  echo "ERROR: The source kit is incomplete. Missing: $TRANSFER_SCRIPT" >&2
  exit 1
fi

exec bash "$TRANSFER_SCRIPT" "$@"
