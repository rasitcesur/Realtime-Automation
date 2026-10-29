#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HEAP_MIN="${HEAP_MIN:-512m}" HEAP_MAX="${HEAP_MAX:-512m}" \
  MAIN_CLASS=alcybe.realtime.CameraDemo exec "$ROOT/scripts/run-linux.sh" "$@"
