#!/usr/bin/env bash
set -euo pipefail
rootDir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${JAVA_HOME:=$(/usr/libexec/java_home -v 21)}"
export JAVA_HOME
mkdir -p "$rootDir/build/classes" "$rootDir/build/test-classes"
find "$rootDir/src/main/java" -name '*.java' -print0 | xargs -0 "$JAVA_HOME/bin/javac" -encoding UTF-8 -d "$rootDir/build/classes"
find "$rootDir/src/test/java" -name '*.java' -print0 | xargs -0 "$JAVA_HOME/bin/javac" -encoding UTF-8 -cp "$rootDir/build/classes" -d "$rootDir/build/test-classes"
cmake -S "$rootDir" -B "$rootDir/build/cmake-macos" -DCMAKE_BUILD_TYPE=Release
cmake --build "$rootDir/build/cmake-macos" --config Release --parallel
