#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
"$ROOT/scripts/build-linux.sh"
JAVA_HOME="${JAVA_HOME:-$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")}"
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror -O2 \
  -I"$ROOT/native" -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/linux" -o "$ROOT/build/native/camera_pixel_test" "$ROOT/tests/native/camera_pixel_test.cpp"
"$ROOT/build/native/camera_pixel_test"
java -XX:+UseZGC -Xms128m -Xmx128m -Djava.library.path="$ROOT/build/native" \
  -cp "$ROOT/build/classes:$ROOT/build/test-classes" alcybe.realtime.CameraApiSmokeTest
