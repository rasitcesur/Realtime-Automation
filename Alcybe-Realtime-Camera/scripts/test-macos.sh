#!/usr/bin/env bash
set -euo pipefail
rootDir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
"$rootDir/scripts/build-macos.sh"
: "${JAVA_HOME:=$(/usr/libexec/java_home -v 21)}"
classpath="$rootDir/build/classes:$rootDir/build/test-classes"
nativePath="$rootDir/build/native"
for testClass in RealtimeSmokeTest CameraApiSmokeTest; do
    "$JAVA_HOME/bin/java" -XX:+UseZGC -Xms128m -Xmx128m \
        -Djava.library.path="$nativePath" -cp "$classpath" "alcybe.realtime.$testClass"
done
for enabled in true false; do
    "$JAVA_HOME/bin/java" -XX:+UseZGC -Xms128m -Xmx128m \
        -Dalcybe.realtime.explicitGcOnIdle="$enabled" \
        -Djava.library.path="$nativePath" -cp "$classpath" alcybe.realtime.RealtimeGcIdleTest
done
