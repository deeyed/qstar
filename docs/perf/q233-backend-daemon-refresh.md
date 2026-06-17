# Q233 Backend And Daemon Performance Refresh

Round Q233는 GLP, real Rust/Zig compiler corpus, Windows process/artifact work 이후
Stella, Stella daemon, Ninja 성능을 다시 찍은 freshness snapshot이다. 이 문서는 release
input이며 stable performance guarantee가 아니다.

## Environment

```txt
date: 2026-06-17
host: macOS arm64
kernel: Darwin 25.5.0
host_jobs: 10
qstar: 0.7.0-beta
baseline commit before docs update: ff32ece
timing policy: report-only
```

## Commands

```sh
make all

QSTAR_TEST_QSTAR=build/bin/qstar \
  sh tests/medium-project-performance.sh

QSTAR_DAEMON_TMPDIR=/private/tmp/qstar-q233-daemon \
QSTAR_TEST_QSTAR=build/bin/qstar \
  sh tests/medium-project-performance.sh

QSTAR_DAEMON_TMPDIR=/private/tmp/qstar-q233-large-daemon \
QSTAR_TEST_QSTAR=build/bin/qstar \
  sh tests/large-project-performance.sh

make qstar-real-glp-compiler-corpus-tests

QSTAR_TEST_QSTAR=build/bin/qstar \
  sh tests/smoke.sh
```

Sandboxed medium run recorded `backend=stella-daemon ... elapsed_ms=skipped
reason=socket-bind-not-permitted`, as expected. The daemon-inclusive medium,
large, and smoke runs above were executed with a socket-permitting local
environment so that the daemon phases did not skip.

## Medium Corpus

The daemon-inclusive medium run is the Q233 medium reference. It exercises 47
targets and records the default scheduler as `runner=posix_spawn event_wait=poll`.

| Gate | Mode | Backend | Phase | Count | Min ms | Median ms | Max ms | Skipped reason |
| --- | --- | --- | --- | ---: | ---: | ---: | ---: | --- |
| medium | medium | stella | clean | 1 | 244 | 244 | 244 |  |
| medium | medium | stella | noop | 1 | 68 | 68 | 68 |  |
| medium | medium | stella | incremental | 1 | 96 | 96 | 96 |  |
| medium | medium | stella-daemon | clean | 1 | 289 | 289 | 289 |  |
| medium | medium | stella-daemon | noop | 1 | 68 | 68 | 68 |  |
| medium | medium | stella-daemon | incremental | 1 | 88 | 88 | 88 |  |
| medium | medium | stella-jobs | clean | 1 | 246 | 246 | 246 |  |
| medium | medium | stella-jobs | noop | 1 | 68 | 68 | 68 |  |
| medium | medium | stella-jobs | incremental | 1 | 89 | 89 | 89 |  |
| medium | medium | ninja | clean | 1 | 254 | 254 | 254 |  |
| medium | medium | ninja | noop | 1 | 74 | 74 | 74 |  |
| medium | medium | ninja | incremental | 1 | 116 | 116 | 116 |  |

| Gate | Mode | Backend | Phase | Backend median ms | Ninja median ms | Ratio | Status |
| --- | --- | --- | --- | ---: | ---: | ---: | --- |
| medium | medium | stella | clean | 244 | 254 | 0.96x | ok |
| medium | medium | stella | noop | 68 | 74 | 0.92x | ok |
| medium | medium | stella | incremental | 96 | 116 | 0.83x | ok |
| medium | medium | stella-daemon | clean | 289 | 254 | 1.14x | ok |
| medium | medium | stella-daemon | noop | 68 | 74 | 0.92x | ok |
| medium | medium | stella-daemon | incremental | 88 | 116 | 0.76x | ok |
| medium | medium | stella-jobs | clean | 246 | 254 | 0.97x | ok |
| medium | medium | stella-jobs | noop | 68 | 74 | 0.92x | ok |
| medium | medium | stella-jobs | incremental | 89 | 116 | 0.77x | ok |

## Large Corpus

The large corpus was run with modes `200 500` and four generated object bridge
actions. The same run measured normal Stella, explicit-jobs Stella, Stella
daemon, and Ninja.

| Gate | Mode | Backend | Phase | Count | Min ms | Median ms | Max ms | Skipped reason |
| --- | --- | --- | --- | ---: | ---: | ---: | ---: | --- |
| large | 200 | stella | clean | 1 | 1094 | 1094 | 1094 |  |
| large | 200 | stella | noop | 1 | 75 | 75 | 75 |  |
| large | 200 | stella | incremental | 1 | 134 | 134 | 134 |  |
| large | 200 | stella-jobs | clean | 1 | 1264 | 1264 | 1264 |  |
| large | 200 | stella-jobs | noop | 1 | 81 | 81 | 81 |  |
| large | 200 | stella-jobs | incremental | 1 | 136 | 136 | 136 |  |
| large | 200 | stella-daemon | clean | 1 | 1179 | 1179 | 1179 |  |
| large | 200 | stella-daemon | noop | 1 | 102 | 102 | 102 |  |
| large | 200 | stella-daemon | incremental | 1 | 126 | 126 | 126 |  |
| large | 200 | ninja | clean | 1 | 1344 | 1344 | 1344 |  |
| large | 200 | ninja | noop | 1 | 105 | 105 | 105 |  |
| large | 200 | ninja | incremental | 1 | 167 | 167 | 167 |  |
| large | 500 | stella | clean | 1 | 2412 | 2412 | 2412 |  |
| large | 500 | stella | noop | 1 | 105 | 105 | 105 |  |
| large | 500 | stella | incremental | 1 | 140 | 140 | 140 |  |
| large | 500 | stella-jobs | clean | 1 | 2451 | 2451 | 2451 |  |
| large | 500 | stella-jobs | noop | 1 | 103 | 103 | 103 |  |
| large | 500 | stella-jobs | incremental | 1 | 144 | 144 | 144 |  |
| large | 500 | stella-daemon | clean | 1 | 2614 | 2614 | 2614 |  |
| large | 500 | stella-daemon | noop | 1 | 107 | 107 | 107 |  |
| large | 500 | stella-daemon | incremental | 1 | 151 | 151 | 151 |  |
| large | 500 | ninja | clean | 1 | 2737 | 2737 | 2737 |  |
| large | 500 | ninja | noop | 1 | 156 | 156 | 156 |  |
| large | 500 | ninja | incremental | 1 | 211 | 211 | 211 |  |

| Gate | Mode | Backend | Phase | Backend median ms | Ninja median ms | Ratio | Status |
| --- | --- | --- | --- | ---: | ---: | ---: | --- |
| large | 200 | stella | clean | 1094 | 1344 | 0.81x | ok |
| large | 200 | stella | noop | 75 | 105 | 0.71x | ok |
| large | 200 | stella | incremental | 134 | 167 | 0.80x | ok |
| large | 200 | stella-jobs | clean | 1264 | 1344 | 0.94x | ok |
| large | 200 | stella-jobs | noop | 81 | 105 | 0.77x | ok |
| large | 200 | stella-jobs | incremental | 136 | 167 | 0.81x | ok |
| large | 200 | stella-daemon | clean | 1179 | 1344 | 0.88x | ok |
| large | 200 | stella-daemon | noop | 102 | 105 | 0.97x | ok |
| large | 200 | stella-daemon | incremental | 126 | 167 | 0.75x | ok |
| large | 500 | stella | clean | 2412 | 2737 | 0.88x | ok |
| large | 500 | stella | noop | 105 | 156 | 0.67x | ok |
| large | 500 | stella | incremental | 140 | 211 | 0.66x | ok |
| large | 500 | stella-jobs | clean | 2451 | 2737 | 0.90x | ok |
| large | 500 | stella-jobs | noop | 103 | 156 | 0.66x | ok |
| large | 500 | stella-jobs | incremental | 144 | 211 | 0.68x | ok |
| large | 500 | stella-daemon | clean | 2614 | 2737 | 0.96x | ok |
| large | 500 | stella-daemon | noop | 107 | 156 | 0.69x | ok |
| large | 500 | stella-daemon | incremental | 151 | 211 | 0.72x | ok |

## Real GLP Corpus

Real Rust/Zig compiler corpus는 backend timing corpus에 섞지 않는다. 이 gate는 실제
compiler를 찾을 수 있을 때 GLP provider final artifact lowering이 Stella/Ninja 양쪽에서
실행되는지만 확인한다.

```txt
real_glp_compiler language=rust backend=stella status=ok compiler=rustc
real_glp_compiler language=rust backend=ninja status=ok compiler=rustc
real_glp_compiler language=zig backend=stella status=ok compiler=zig
real_glp_compiler language=zig backend=ninja status=ok compiler=zig
real_glp_compiler language=zig fixture=executable backend=stella status=ok compiler=zig
real_glp_compiler language=zig fixture=executable backend=ninja status=ok compiler=zig
real_glp_compiler_corpus status=ok
```

## Interpretation

- GLP and Windows work did not introduce a visible local backend regression in
  this snapshot.
- Medium normal Stella is slightly faster than Ninja in all three phases
  (`0.96x`, `0.92x`, `0.83x`). Medium daemon clean has lifecycle/IPC overhead
  and is slower than Ninja (`1.14x`), but daemon no-op and incremental are at or
  ahead of Ninja.
- Large normal Stella, explicit-jobs Stella, and Stella daemon are all at or
  ahead of Ninja in the measured 200/500 target modes.
- Daemon remains an IDE/build-service path, not a guaranteed faster clean-build
  path. Its value is resident state, watcher invalidation, and stable low
  latency on repeated requests.
- Timing remains host-sensitive and report-only. Release gates should cite
  median values from repeat runs when making public performance claims.
