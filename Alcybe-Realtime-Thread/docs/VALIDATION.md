# Test and validation record

Local build environment: Linux x86-64, OpenJDK 21.0.12.1, GCC/G++ 14.2.0.

Validated in the build container:

- Compiled Java source and native JNI library with `-std=c++17 -O3 -march=native -flto -Wall -Wextra -Werror -shared -fPIC -pthread`.
- Ran the executable `alcybe.realtime.MainApp` with JDK 21 ZGC flags and a small test heap.
- Ran the JNI integration test suite for clocks/sleep, memory alignment and bounds, one-shot and periodic execution, pause/resume/stop/join, callback error, PI mutex, 2-party barrier, executor service, double-launch handling, and repeated creation/cleanup.
- No privileges were required for the default-priority tests.

Not validated here:

- Native NUMA build (the validation host has the `libnuma.so.1` runtime, but **not** the `libnuma-dev` header/linker package); this path has a CI job configured to compile and test on Ubuntu with `libnuma-dev`.
- Real-time priority 1..99, CPU pinning to isolated cores, multi-node memory residency, huge pages, and 8 GB heap initialization.
- Hard real-time bounds, p99.9 latency, and hardware-specific jitter; these require measurements on the user's target Linux machine.

Test success means functional integration on this container, **not** real-time certification.

Final verification on 2026-10-28:

- Repeated `./scripts/test-linux.sh` **three times**: each run reported `PASS: 133 JNI smoke-test assertions`.
- `HEAP_MIN=128m HEAP_MAX=128m ./scripts/run-linux.sh`: completed 100 periodic cycles with ZGC.
- `./scripts/bench-linux.sh 1000`: completed; recorded p50/p99/p99.9 from 900 post-warmup samples. Quantiles varied between runs and are not portable performance claims.
