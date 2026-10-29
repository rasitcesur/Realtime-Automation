#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ ! -f "$ROOT/build/native/librealtime.so" ]]; then "$ROOT/scripts/build-linux.sh"; fi
JAVA_BIN="${JAVA_BIN:-java}"
version="$("$JAVA_BIN" -version 2>&1 | head -n 1 | sed -E 's/.*version "([0-9]+).*/\1/; t; s/.*openjdk ([0-9]+).*/\1/')"
[[ "$version" =~ ^[0-9]+$ ]] || { echo "Cannot detect Java version" >&2; exit 1; }
if (( version < 21 )); then echo "JDK 21 or later required" >&2; exit 1; fi
flags=(-XX:+UseZGC -XX:+AlwaysPreTouch -XX:+UseNUMA -XX:+ExplicitGCInvokesConcurrent)
# JDK 23+ defaults to generational ZGC; 24+ obsoleted -XX:+ZGenerational.
if (( version <= 22 )); then flags+=(-XX:+ZGenerational); fi
if [[ "${USE_LARGE_PAGES:-0}" == 1 ]]; then flags+=(-XX:+UseLargePages); fi
read -r -a EXTRA <<< "${JAVA_EXTRA_OPTS:-}"
exec "$JAVA_BIN" "${flags[@]}" -Xms"${HEAP_MIN:-8G}" -Xmx"${HEAP_MAX:-8G}" \
  "${EXTRA[@]}" -Djava.library.path="$ROOT/build/native" \
  -cp "$ROOT/build/classes" "${MAIN_CLASS:-alcybe.realtime.MainApp}" "$@"
