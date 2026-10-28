#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${CXX:=g++}"
: "${JAVAC:=javac}"
if [[ -z "${JAVA_HOME:-}" ]]; then
  JAVA_HOME="$(dirname "$(dirname "$(readlink -f "$(command -v "$JAVAC")")")")"
fi
[[ -f "$JAVA_HOME/include/jni.h" ]] || { echo "ERROR: JAVA_HOME/include/jni.h missing" >&2; exit 1; }
mkdir -p "$ROOT/build/classes" "$ROOT/build/test-classes" "$ROOT/build/native"
mapfile -t JAVA_SOURCES < <(find "$ROOT/src/main/java" -type f -name '*.java' | sort)
mapfile -t TEST_SOURCES < <(find "$ROOT/src/test/java" -type f -name '*.java' | sort)
"$JAVAC" -encoding UTF-8 -d "$ROOT/build/classes" "${JAVA_SOURCES[@]}"
"$JAVAC" -encoding UTF-8 -cp "$ROOT/build/classes" -d "$ROOT/build/test-classes" "${TEST_SOURCES[@]}"
flags=(-std=c++17 -O3 -flto -fPIC -shared -pthread -Wall -Wextra -Werror)
if [[ "${MARCH_NATIVE:-1}" == 1 ]]; then flags+=(-march=native); fi
libs=(-lrt)
if [[ -f /usr/include/numa.h ]] && (ldconfig -p 2>/dev/null | grep -q 'libnuma.so.1') && "${CXX}" -print-file-name=libnuma.so | grep -q '/'; then
  flags+=(-DALCYBE_WITH_NUMA=1)
  libs+=(-lnuma)
  echo "Building with libnuma support"
else
  echo "Building without NUMA support (install libnuma-dev to enable)"
fi
"$CXX" "${flags[@]}" -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/linux" \
  "$ROOT/native/alcybe_realtime_RealtimeThread.cpp" -o "$ROOT/build/native/librealtime.so" "${libs[@]}"
echo "Built $ROOT/build/native/librealtime.so"
