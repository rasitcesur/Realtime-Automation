#!/usr/bin/env bash
set -euo pipefail
rootDir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
"$rootDir/scripts/build-macos.sh"
: "${JAVA_HOME:=$(/usr/libexec/java_home -v 21)}"
javaFlags=(-XX:+UseZGC -XX:+ExplicitGCInvokesConcurrent)
javaVersion=$("$JAVA_HOME/bin/java" -version 2>&1 | head -1 | sed -E 's/.*version "([0-9]+).*/\1/')
if [[ "$javaVersion" =~ ^[0-9]+$ ]] && (( javaVersion <= 22 )); then
    javaFlags+=(-XX:+ZGenerational)
fi
"$JAVA_HOME/bin/java" "${javaFlags[@]}" -Xms256m -Xmx2G \
    -Djava.library.path="$rootDir/build/native" \
    -cp "$rootDir/build/classes" alcybe.realtime.CameraDemo \
    "${1:-default}" "${2:-640}" "${3:-480}" "${4:-30}" "${5:-100}" ARGB
