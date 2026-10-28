#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
"$ROOT/scripts/build-linux.sh"
java -XX:+UseZGC -Xms128m -Xmx128m \
  -Djava.library.path="$ROOT/build/native" \
  -cp "$ROOT/build/classes:$ROOT/build/test-classes" alcybe.realtime.RealtimeSmokeTest

# Both modes: preserve 1-ms idle threshold and allow opt-out.
java -XX:+UseZGC -XX:+ExplicitGCInvokesConcurrent -Xms128m -Xmx128m \
  -Djava.library.path="$ROOT/build/native" -Dalcybe.realtime.explicitGcOnIdle=true \
  -cp "$ROOT/build/classes:$ROOT/build/test-classes" alcybe.realtime.RealtimeGcIdleTest
java -XX:+UseZGC -Xms128m -Xmx128m \
  -Djava.library.path="$ROOT/build/native" -Dalcybe.realtime.explicitGcOnIdle=false \
  -cp "$ROOT/build/classes:$ROOT/build/test-classes" alcybe.realtime.RealtimeGcIdleTest
