# Performance Gates

QStar는 Stella executor, experimental Stella daemon path, Ninja backend를 비교하는
medium/large project gate를 가진다. 목표는 “빠른 것 같다”가 아니라 clean, no-op,
incremental build 시간을 line protocol로 남기고 release마다 추적하는 것이다.

## 실행

```sh
make qstar-medium-project-readiness-tests
```

Generic DSL hard cut 이후 release 후보는 backend/performance seal도 함께 실행한다.

```sh
make qstar-generic-dsl-backend-parity-tests
```

이 gate는 self-host, Stella generated/object/sharedlib smoke, Ninja backend parity,
Linux validation, medium Stella/Ninja timing summary를 current `qstar.toolset` /
`qstar.config` authoring surface로 묶는다. 구조적 backend failure는 hard fail이고,
timing은 기본적으로 Stella/Ninja `2.5x + 500ms` hard threshold를 넘으면 실패한다.

또는 QStar binary를 직접 지정한다.

```sh
QSTAR_TEST_QSTAR=./build/bin/qstar sh tests/medium-project-performance.sh
```

사용자가 직접 돌릴 수 있는 정적 fixture도 있다.

```sh
./build/bin/qstar --file tests/corpus/medium/qstar.lua build //:all
./build/bin/qstar --file tests/corpus/medium/qstar.lua build //:all
```

## 출력 포맷

```txt
medium_project_gate scheduler host_jobs=10
medium_project_gate scheduler default_jobs=10 ready_width=40 async_final_actions=40 trace_elapsed_ms=257
medium_project_gate scheduler runner=posix_spawn event_wait=poll
medium_project_gate backend=stella phase=clean elapsed_ms=123
medium_project_gate backend=stella phase=noop elapsed_ms=42
medium_project_gate backend=stella phase=incremental elapsed_ms=51
medium_project_gate backend=stella-daemon phase=clean elapsed_ms=110 cli_clean_ms=123
medium_project_gate backend=stella-daemon phase=noop elapsed_ms=21 cli_noop_ms=42
medium_project_gate backend=stella-daemon phase=incremental elapsed_ms=36 cli_incremental_ms=51
medium_project_gate backend=stella-jobs jobs=10 phase=clean elapsed_ms=118
medium_project_gate staticlib_argv_parity=ok target=//modules/core/cache:module_cache
medium_project_gate backend=ninja phase=clean elapsed_ms=100
medium_project_gate backend=ninja phase=noop elapsed_ms=30
medium_project_gate backend=ninja phase=incremental elapsed_ms=36
medium_project_gate compare phase=clean stella_ms=123 ninja_ms=100 ratio_x100=200 slack_ms=250
medium_project_gate compare backend=stella-daemon phase=noop stella_ms=21 ninja_ms=30 ratio_x100=200 slack_ms=250
medium_project_gate status=ok perf_issue_count=0 report_only=1
```

Ninja가 없으면 Ninja phase는 `skipped`로 표시된다. Stella phase는 항상 실행된다. Daemon
socket bind가 sandbox나 host policy 때문에 불가능하면 daemon phase는
`elapsed_ms=skipped reason=socket-bind-not-permitted`로 표시된다.

## Phase

| Phase | 의미 |
| --- | --- |
| `clean` | 빈 build directory에서 처음 build |
| `noop` | 변경 없이 같은 build directory에서 재실행 |
| `incremental` | 하나의 C source만 바꾼 뒤 재실행 |

## 목표

- no-op build는 0.2초대에 접근한다.
- clean build는 Ninja 대비 1.5-2배 이내를 목표로 한다.
- incremental build는 변경된 action만 다시 실행되도록 유지한다.

## 최신 베타 스냅샷

Round Q137 local macOS arm64 대표 측정값:

```txt
medium_project_gate target_count=47 min_targets=40
medium_project_gate scheduler host_jobs=10
medium_project_gate scheduler default_jobs=10 ready_width=40 async_final_actions=40 trace_elapsed_ms=257
medium_project_gate scheduler runner=posix_spawn event_wait=poll
medium_project_gate backend=stella phase=clean elapsed_ms=247
medium_project_gate backend=stella phase=noop elapsed_ms=67
medium_project_gate backend=stella phase=incremental elapsed_ms=89
medium_project_gate backend=stella-jobs jobs=10 phase=clean elapsed_ms=237
medium_project_gate backend=stella-jobs jobs=10 phase=noop elapsed_ms=68
medium_project_gate backend=stella-jobs jobs=10 phase=incremental elapsed_ms=88
medium_project_gate staticlib_argv_parity=ok target=//modules/core/cache:module_cache
medium_project_gate backend=ninja phase=clean elapsed_ms=251
medium_project_gate backend=ninja phase=noop elapsed_ms=73
medium_project_gate backend=ninja phase=incremental elapsed_ms=97
medium_project_gate compare phase=clean stella_ms=247 ninja_ms=251 ratio_x100=200 slack_ms=250
medium_project_gate compare phase=noop stella_ms=67 ninja_ms=73 ratio_x100=200 slack_ms=250
medium_project_gate compare phase=incremental stella_ms=89 ninja_ms=97 ratio_x100=200 slack_ms=250
medium_project_gate compare backend=stella-jobs phase=clean stella_ms=237 ninja_ms=251 ratio_x100=200 slack_ms=250
medium_project_gate compare backend=stella-jobs phase=noop stella_ms=68 ninja_ms=73 ratio_x100=200 slack_ms=250
medium_project_gate compare backend=stella-jobs phase=incremental stella_ms=88 ninja_ms=97 ratio_x100=200 slack_ms=250
medium_project_gate status=ok perf_issue_count=0 report_only=1
```

Old/new comparison:

| Snapshot | Stella clean | Stella no-op | Stella incremental | Ninja clean | Ninja no-op | Ninja incremental |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Q132 representative | 991ms | 79ms | 100ms | 276ms | 89ms | 107ms |
| Q137 default Stella | 247ms | 67ms | 89ms | 251ms | 73ms | 97ms |
| Q137 explicit jobs | 237ms | 68ms | 88ms | 251ms | 73ms | 97ms |

Stella no-op과 incremental은 이 corpus에서 Ninja급 latency를 보인다. Q125는 clean
build 중 state/deps/action metadata write path를 buffered write로 정리하고, build
directory 내부 generated object/archive input은 content hash 대신 size/mtime metadata
key로 다룬다. Q129는 compile/archive/link/custom generated action 실행 경로에 POSIX spawn
runner를 추가했다. macOS와 Linux/glibc는 `posix_spawn` fast path를 사용하고, unsupported
platform이나 spawn setup failure는 기존 fork/exec path로 fallback한다. Q130은 compile/custom
action wait loop에서 fixed sleep pause를 제거하고 stdout/stderr pipe readiness를 POSIX
`poll()`로 기다린다. Q131은 successful/cache-hit action log를 lazy materialization으로
전환해 clean build metadata file write 수를 줄인다. Q132는 `state.db`를 Stella
dirty-check의 canonical fast path로 명확화하고, 사람이 읽는 `state/actions.json` dump를
`QSTAR_DEBUG_STATE_DUMPS=1` opt-in으로 내려 fast path에서 JSON debug export write를 제거했다.
Q133-Q136은 macOS default jobs 감지, staticlib dependency archive nesting 제거, compile
dependency edge 완화, archive/link final action async scheduling을 더했다. Q140은 external
generated action과 run action도 prepared-action scheduler path에 올렸고, Q141은
`state/graph.json`과 성공 `state/last-summary.json`을 debug/export opt-in으로 내려 일반
clean build의 metadata write를 더 줄였다.

Q137 대표 측정에서는 Stella clean이 Ninja 대비 2배 이내 목표를 넘어 1.5배 이내에 들어왔다.
하지만 timing은 host CPU, filesystem cache, compiler warm state에 흔들리므로, 이 수치를
stable 성능 보장으로 선언하지 않는다. Gate는 default jobs, ready queue width,
async final action count, staticlib argv parity를 hard check하고, timing ratio는 report-only로
유지한다.

남은 병목은 더 큰 corpus에서의 compiler process count, plan cache/store 비용,
host별 process runner 편차다. 다음 목표는 medium gate에서 Stella clean을 Ninja 대비
1.5배 이내에 더 안정적으로 유지하고, 더 큰 synthetic
corpus에서도 같은 경향이 나오는지 확인하는 것이다.

Round Q92 기준 timing threshold는 기본적으로 report-only다. 파일 누락, graph 실패,
compiler 실패, compile database 누락은 hard fail이고, 시간 초과는 warning으로 기록된다.

Hard gate로 실행하려면 다음처럼 한다.

```sh
QSTAR_MEDIUM_PERF_REPORT_ONLY=0 make qstar-medium-project-readiness-tests
```

주요 tuning variable:

- `QSTAR_MEDIUM_CLEAN_MAX_MS`
- `QSTAR_MEDIUM_NOOP_MAX_MS`
- `QSTAR_MEDIUM_INCREMENTAL_MAX_MS`
- `QSTAR_MEDIUM_STELLA_TO_NINJA_X100`
- `QSTAR_MEDIUM_RATIO_SLACK_MS`
- `QSTAR_MEDIUM_MIN_TARGETS`

## Perf Summary Tool

Q139부터 raw line protocol은 `tools/perf-summary.sh`로 요약한다. 이 도구는
`medium_project_gate`와 `large_project_gate`를 모두 읽고, 같은 gate/mode/backend/phase
sample의 `min`, `median`, `max`와 Stella/Ninja ratio를 계산한다.

```sh
QSTAR_TEST_QSTAR=./build/bin/qstar sh tests/medium-project-performance.sh \
  > /tmp/qstar-medium.perf
tools/perf-summary.sh /tmp/qstar-medium.perf
```

대표 출력:

```txt
perf_summary sample gate=medium mode=medium backend=stella phase=clean count=3 min_ms=237 median_ms=247 max_ms=260 skipped_reason=
perf_summary sample gate=medium mode=medium backend=stella-daemon phase=noop count=3 min_ms=20 median_ms=22 max_ms=25 skipped_reason=
perf_summary sample gate=medium mode=medium backend=stella-daemon phase=clean count=1 min_ms=skipped median_ms=skipped max_ms=skipped skipped_reason=socket-bind-not-permitted
perf_summary ratio gate=medium mode=medium backend=stella phase=clean backend_median_ms=247 ninja_median_ms=251 ratio_x100=98 threshold_x100=200 slack_ms=250 warn_threshold_x100=200 warn_slack_ms=250 hard_threshold_x100=200 hard_slack_ms=250 status=ok
perf_summary ratio gate=medium mode=medium backend=stella-daemon phase=noop backend_median_ms=22 ninja_median_ms=73 ratio_x100=30 threshold_x100=200 slack_ms=250 warn_threshold_x100=200 warn_slack_ms=250 hard_threshold_x100=200 hard_slack_ms=250 status=ok
perf_summary status=ok sample_count=12 skipped_count=1 ratio_count=9 warning_count=0 hard_failure_count=0 hard=0 threshold_x100=200 slack_ms=250 hard_threshold_x100=200 hard_slack_ms=250
```

Release note용 표는 markdown format으로 만든다.

```sh
tools/perf-summary.sh --format markdown --label "QStar perf snapshot" \
  /tmp/qstar-medium.perf
```

Markdown output은 macOS local run과 Linux CI artifact가 같은 column을 쓴다. Daemon이
sandbox나 host policy 때문에 실행되지 않았으면 numeric cell을 `-`로 두고
`Skipped reason` column에 이유를 남긴다.

```md
| Gate | Mode | Backend | Phase | Count | Min ms | Median ms | Max ms | Skipped reason |
| --- | --- | --- | --- | ---: | ---: | ---: | ---: | --- |
| medium | medium | stella | clean | 3 | 237 | 247 | 260 |  |
| medium | medium | stella-daemon | clean | 1 | - | - | - | socket-bind-not-permitted |

| Gate | Mode | Backend | Phase | Backend median ms | Ninja median ms | Ratio | Warn threshold | Hard threshold | Status |
| --- | --- | --- | --- | ---: | ---: | ---: | --- | --- | --- |
| medium | medium | stella | clean | 247 | 251 | 0.98x | 2.00x + 250ms | 2.00x + 250ms | ok |
```

반복 측정은 `--repeat`로 한다. `--` 뒤 command는 shell string이 아니라 argv-vector다.

```sh
tools/perf-summary.sh --repeat 3 -- \
  env QSTAR_TEST_QSTAR=./build/bin/qstar sh tests/medium-project-performance.sh
```

기본은 report-only다. Warning threshold는 release note와 CI artifact에서 주의가 필요한
수치를 표시하고, hard threshold는 `--hard`와 함께 쓸 때 exit failure 기준이 된다.
`--ratio-x100`과 `--slack-ms`는 warning threshold compatibility alias다.

```sh
tools/perf-summary.sh \
  --warn-ratio-x100 150 --warn-slack-ms 0 \
  --hard-ratio-x100 200 --hard-slack-ms 250 \
  /tmp/qstar-medium.perf

tools/perf-summary.sh \
  --warn-ratio-x100 150 --warn-slack-ms 0 \
  --hard-ratio-x100 200 --hard-slack-ms 250 \
  --hard /tmp/qstar-medium.perf
```

Release gate artifact를 한 번에 만들려면 다음 target을 쓴다.

```sh
QSTAR_PERF_REPEAT=3 \
QSTAR_PERF_ARTIFACT_DIR=dist/perf \
make qstar-performance-release-gate
```

Q176 release gate format:

```txt
dist/perf/medium-release-raw.txt
dist/perf/medium-release-summary.txt
dist/perf/medium-release-summary.md
dist/perf/large-release-raw.txt
dist/perf/large-release-summary.txt
dist/perf/large-release-summary.md
```

| Snapshot | Host | Corpus | Repeat | Stella clean | Stella no-op | Stella incremental | Ninja clean | Ninja no-op | Ninja incremental | Daemon |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| Q166 | macOS arm64 | large 200 | 3 | 1054ms | 80ms | 137ms | 1220ms | 102ms | 154ms | measured |
| Q166 | macOS arm64 | large 500 | 3 | 2437ms | 101ms | 145ms | 2816ms | 144ms | 228ms | measured |
| Q169 | macOS arm64 | medium 47 | 1 | 236ms | 76ms | 91ms | 269ms | 74ms | 103ms | skipped: socket bind |
| Q176 dry run | macOS arm64 | medium 47 | 1 | 269ms | 68ms | 89ms | 252ms | 72ms | 97ms | skipped: socket bind |
| Q176 dry run | macOS arm64 | large 200 | 1 | 984ms | 72ms | 114ms | 1012ms | 92ms | 141ms | skipped: socket bind |

Q176 dry run은 `QSTAR_PERF_REPEAT=1 QSTAR_LARGE_PROJECT_TARGETS=200`으로 release artifact
format을 검증한 값이다. Public release note에는 repeat-3 또는 hosted CI artifact를 우선한다.

## Linux CI Performance Artifacts

Round Q142부터 Linux validation workflow는 Ubuntu gcc/clang matrix에서 medium corpus
Stella/Ninja timing을 line protocol로 수집한다. 이 결과는 Linux에서 macOS 대표 수치와 같은
성능 경향이 재현되는지 보기 위한 artifact이며, timing threshold는 아직 report-only다.

Workflow hard check는 다음을 확인한다.

- Stella medium build가 `medium_project_gate ...` output을 남긴다.
- Ninja backend clean phase가 같은 output에 포함된다.
- Linux Stella scheduler trace가 POSIX fast path를 보고한다.

```txt
medium_project_gate scheduler runner=posix_spawn event_wait=poll
medium_project_gate backend=ninja phase=clean elapsed_ms=...
```

업로드되는 medium artifact:

```txt
dist/perf/linux-<compiler>-medium-perf.txt
dist/perf/linux-<compiler>-medium-summary.txt
dist/perf/linux-<compiler>-medium-summary.md
```

Large Synthetic Corpus는 기본 push/PR lane이 아니라 `workflow_dispatch` 전용
`large-performance-report` job에서 report-only artifact로 수집한다.

```txt
dist/perf/linux-gcc-large-perf.txt
dist/perf/linux-gcc-large-summary.txt
dist/perf/linux-gcc-large-summary.md
```

## Large Synthetic Corpus

Q138부터 large synthetic corpus gate를 별도로 둔다. Medium gate가 beta readiness 대표값이라면,
large gate는 Stella/Ninja scheduler scaling을 보기 위한 report-only 성능 입력이다.

```sh
QSTAR_TEST_QSTAR=./build/bin/qstar sh tests/large-project-performance.sh
```

Makefile target도 있다.

```sh
make qstar-large-project-performance-tests
```

기본 mode는 200 target과 500 target이다.

```sh
QSTAR_LARGE_PROJECT_TARGETS="200 500" sh tests/large-project-performance.sh
```

빠른 local 확인은 한 mode만 돌린다.

```sh
QSTAR_LARGE_PROJECT_TARGETS=200 QSTAR_TEST_QSTAR=./build/bin/qstar \
  sh tests/large-project-performance.sh
```

Large corpus는 staticlib fanout, mode별 executable link shard, `qstar.group "all"`,
`qstar.custom_target` 기반 object artifact bridge를 포함한다. Object artifact bridge는
`qstar.output(..., {format = "object"})`로 생성된 object file을 staticlib source로 소비하고,
executable shard가 그 staticlib를 link dependency로 받는 구조다. Large gate도 Stella default,
Stella explicit `--jobs`, Stella daemon, Ninja의 clean/no-op/incremental phase를 같은 line
protocol로 기록한다.

각 backend는 동일한 synthetic source tree를 별도 임시 root에 생성해 측정한다. Project-level
`generated_dir`가 backend 간에 공유되어 clean 비교가 흐려지는 일을 피하기 위해서다.

출력 prefix는 `large_project_gate`다.

```txt
large_project_gate mode=200 target_count=200 generated_actions=4 host_jobs=10
large_project_gate mode=200 backend=stella phase=clean elapsed_ms=1234
large_project_gate mode=200 backend=stella-jobs jobs=10 phase=clean elapsed_ms=1200
large_project_gate mode=200 backend=stella-daemon phase=clean elapsed_ms=1040
large_project_gate mode=200 backend=stella-daemon phase=noop elapsed_ms=30
large_project_gate mode=200 backend=stella-daemon phase=incremental elapsed_ms=70
large_project_gate mode=200 backend=ninja phase=clean elapsed_ms=900
large_project_gate mode=200 compare phase=clean stella_ms=1234 ninja_ms=900 ratio_x100=200 slack_ms=500
large_project_gate mode=200 compare backend=stella-daemon phase=noop stella_ms=30 ninja_ms=96 ratio_x100=200 slack_ms=500
large_project_gate status=ok perf_issue_count=0 report_only=1 modes="200 500"
```

Daemon socket bind가 불가능한 sandbox에서는 daemon backend만 skipped로 기록하고 나머지 backend는
계속 측정한다.

Large gate의 timing threshold는 기본적으로 report-only다. Graph/build failure,
compile database 누락, generated object bridge 누락, Ninja root `.ninja_log`/`.ninja_deps`
오염은 hard fail이다. Large 결과는 stable 성능 보장이 아니라 representative local/CI run으로
해석한다.

Round Q166 local macOS arm64 repeat-3 대표 측정값:

```txt
large_project_gate mode=200 target_count=200 generated_actions=4 host_jobs=10
perf_summary sample gate=large mode=200 backend=stella phase=clean count=3 min_ms=1006 median_ms=1054 max_ms=3553
perf_summary sample gate=large mode=200 backend=stella phase=noop count=3 min_ms=79 median_ms=80 max_ms=102
perf_summary sample gate=large mode=200 backend=stella phase=incremental count=3 min_ms=128 median_ms=137 max_ms=148
perf_summary sample gate=large mode=200 backend=stella-daemon phase=clean count=3 min_ms=1056 median_ms=1389 max_ms=1543
perf_summary sample gate=large mode=200 backend=ninja phase=clean count=3 min_ms=1127 median_ms=1220 max_ms=3714
perf_summary sample gate=large mode=200 backend=ninja phase=noop count=3 min_ms=98 median_ms=102 max_ms=103
perf_summary sample gate=large mode=200 backend=ninja phase=incremental count=3 min_ms=153 median_ms=154 max_ms=157
large_project_gate mode=500 target_count=500 generated_actions=4 host_jobs=10
perf_summary sample gate=large mode=500 backend=stella phase=clean count=3 min_ms=2312 median_ms=2437 max_ms=4723
perf_summary sample gate=large mode=500 backend=stella phase=noop count=3 min_ms=98 median_ms=101 max_ms=104
perf_summary sample gate=large mode=500 backend=stella phase=incremental count=3 min_ms=140 median_ms=145 max_ms=160
perf_summary sample gate=large mode=500 backend=stella-daemon phase=clean count=3 min_ms=2540 median_ms=2601 max_ms=2726
perf_summary sample gate=large mode=500 backend=ninja phase=clean count=3 min_ms=2504 median_ms=2816 max_ms=4311
perf_summary sample gate=large mode=500 backend=ninja phase=noop count=3 min_ms=142 median_ms=144 max_ms=369
perf_summary sample gate=large mode=500 backend=ninja phase=incremental count=3 min_ms=207 median_ms=228 max_ms=344
perf_summary status=ok sample_count=24 ratio_count=18 warning_count=0 hard=0 threshold_x100=200 slack_ms=250
```

Normal Stella는 200/500 target mode 모두 clean, no-op, incremental median에서 Ninja보다
빠르게 측정됐다. Stella daemon은 clean build에서 normal Stella보다 빠르지는 않지만,
no-op/incremental은 같은 latency band에 있다. Max 값은 host noise로 크게 흔들릴 수 있으므로
release note에는 median 중심으로만 인용한다.
