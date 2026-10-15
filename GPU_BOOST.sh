#!/usr/bin/env bash
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
BOOST_SCRIPT="$ROOT/tools/gpu-boost.sh"

if [ ! -f "$BOOST_SCRIPT" ]; then
  echo "ERROR: The source kit is incomplete. Missing: $BOOST_SCRIPT" >&2
  exit 1
fi

exec bash "$BOOST_SCRIPT" "$@"
