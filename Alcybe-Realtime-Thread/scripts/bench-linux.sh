#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
"$ROOT/scripts/build-linux.sh"
java -XX:+UseZGC -Xms128m -Xmx128m \
  -Djava.library.path="$ROOT/build/native" \
  -cp "$ROOT/build/classes:$ROOT/build/test-classes" alcybe.realtime.RealtimeBenchmark "$@"
