#!/usr/bin/env bash
set -euo pipefail
rootDir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
"$rootDir/scripts/build-linux.sh"
: "${JAVA_HOME:=$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")}" 
export JAVA_HOME
cmake -S "$rootDir" -B "$rootDir/build/cmake-mock" \
  -DCMAKE_BUILD_TYPE=Release -DALCYBE_PORTABLE_TEST_BACKEND=ON \
  -DCMAKE_LIBRARY_OUTPUT_DIRECTORY="$rootDir/build/mock-native"
cmake --build "$rootDir/build/cmake-mock" --parallel
nativePath="$rootDir/build/mock-native"
classpath="$rootDir/build/classes:$rootDir/build/test-classes"
java -XX:+UseZGC -Xms128m -Xmx128m \
  -Djava.library.path="$nativePath" -cp "$classpath" alcybe.realtime.RealtimeSmokeTest
java -XX:+UseZGC -Xms128m -Xmx128m \
  -Djava.library.path="$nativePath" -cp "$classpath" alcybe.realtime.PortableCameraIntegrationTest
java -XX:+UseZGC -Xms128m -Xmx128m \
  -Djava.library.path="$nativePath" -cp "$classpath" alcybe.realtime.CameraApiSmokeTest
