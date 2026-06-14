# Q166 Large Performance Refresh

Round Q166 refreshes the large synthetic Stella/Ninja benchmark for the 0.7
readiness gate. The large gate is a report-only scaling input, not a stable
performance guarantee.

## Command

```sh
tools/perf-summary.sh --format markdown \
  --label "Q166 local macOS arm64 large synthetic repeat-3" \
  --repeat 3 -- \
  env QSTAR_TEST_QSTAR=build/bin/qstar sh tests/large-project-performance.sh
```

The first sandboxed run skipped `stella-daemon` because Unix socket bind was not
permitted. The repeat-3 run below was executed outside the sandbox so daemon
socket setup could be measured.

## Environment

```txt
host: macOS arm64
host_jobs: 10
modes: 200 500
object bridge generated actions: 4
timing policy: report-only
summary: repeat-3 min/median/max
```

## Summary

| Gate | Mode | Backend | Phase | Count | Min ms | Median ms | Max ms |
| --- | --- | --- | --- | ---: | ---: | ---: | ---: |
| large | 200 | stella | clean | 3 | 1006 | 1054 | 3553 |
| large | 200 | stella | noop | 3 | 79 | 80 | 102 |
| large | 200 | stella | incremental | 3 | 128 | 137 | 148 |
| large | 200 | stella-jobs | clean | 3 | 1497 | 1965 | 4137 |
| large | 200 | stella-jobs | noop | 3 | 79 | 81 | 83 |
| large | 200 | stella-jobs | incremental | 3 | 125 | 125 | 146 |
| large | 200 | stella-daemon | clean | 3 | 1056 | 1389 | 1543 |
| large | 200 | stella-daemon | noop | 3 | 81 | 95 | 97 |
| large | 200 | stella-daemon | incremental | 3 | 125 | 129 | 178 |
| large | 200 | ninja | clean | 3 | 1127 | 1220 | 3714 |
| large | 200 | ninja | noop | 3 | 98 | 102 | 103 |
| large | 200 | ninja | incremental | 3 | 153 | 154 | 157 |
| large | 500 | stella | clean | 3 | 2312 | 2437 | 4723 |
| large | 500 | stella | noop | 3 | 98 | 101 | 104 |
| large | 500 | stella | incremental | 3 | 140 | 145 | 160 |
| large | 500 | stella-jobs | clean | 3 | 2440 | 2585 | 3015 |
| large | 500 | stella-jobs | noop | 3 | 101 | 104 | 107 |
| large | 500 | stella-jobs | incremental | 3 | 136 | 143 | 145 |
| large | 500 | stella-daemon | clean | 3 | 2540 | 2601 | 2726 |
| large | 500 | stella-daemon | noop | 3 | 105 | 107 | 107 |
| large | 500 | stella-daemon | incremental | 3 | 147 | 151 | 226 |
| large | 500 | ninja | clean | 3 | 2504 | 2816 | 4311 |
| large | 500 | ninja | noop | 3 | 142 | 144 | 369 |
| large | 500 | ninja | incremental | 3 | 207 | 228 | 344 |

| Gate | Mode | Backend | Phase | Backend median ms | Ninja median ms | Ratio | Status |
| --- | --- | --- | --- | ---: | ---: | ---: | --- |
| large | 200 | stella | clean | 1054 | 1220 | 0.86x | ok |
| large | 200 | stella | noop | 80 | 102 | 0.78x | ok |
| large | 200 | stella | incremental | 137 | 154 | 0.89x | ok |
| large | 200 | stella-jobs | clean | 1965 | 1220 | 1.61x | ok |
| large | 200 | stella-jobs | noop | 81 | 102 | 0.79x | ok |
| large | 200 | stella-jobs | incremental | 125 | 154 | 0.81x | ok |
| large | 200 | stella-daemon | clean | 1389 | 1220 | 1.14x | ok |
| large | 200 | stella-daemon | noop | 95 | 102 | 0.93x | ok |
| large | 200 | stella-daemon | incremental | 129 | 154 | 0.84x | ok |
| large | 500 | stella | clean | 2437 | 2816 | 0.87x | ok |
| large | 500 | stella | noop | 101 | 144 | 0.70x | ok |
| large | 500 | stella | incremental | 145 | 228 | 0.64x | ok |
| large | 500 | stella-jobs | clean | 2585 | 2816 | 0.92x | ok |
| large | 500 | stella-jobs | noop | 104 | 144 | 0.72x | ok |
| large | 500 | stella-jobs | incremental | 143 | 228 | 0.63x | ok |
| large | 500 | stella-daemon | clean | 2601 | 2816 | 0.92x | ok |
| large | 500 | stella-daemon | noop | 107 | 144 | 0.74x | ok |
| large | 500 | stella-daemon | incremental | 151 | 228 | 0.66x | ok |

## Interpretation

- Normal Stella is at or ahead of Ninja on this local repeat-3 large corpus:
  clean ratio is `0.86x` for 200 targets and `0.87x` for 500 targets.
- No-op and incremental phases stay below Ninja median for both target sizes.
- Stella daemon is useful as an IDE/build-service path, but this benchmark does
  not show a clean-build win over normal Stella. Its no-op and incremental
  phases are still in the same latency band as normal Stella and Ninja.
- The large gate remains report-only. The wide max values show host noise, so
  release notes should cite median values and avoid stable performance promises.
