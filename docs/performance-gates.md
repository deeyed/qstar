# QStar Performance Gates

QStar는 Stella executor와 Ninja backend를 같은 medium project shape에서 비교해
executor 품질을 숫자로 관리한다. 이 문서는 release gate의 개발자용 계약이다. 사용자
문서는 `wiki/reference/performance-gates.md`를 함께 갱신한다.

## Medium Corpus Gate

기본 gate는 다음 명령으로 실행한다.

```sh
make qstar-medium-project-readiness-tests
```

이 gate는 임시 project를 만들고 다음 phase를 측정한다.

| Phase | 의미 |
| --- | --- |
| `clean` | 빈 build directory에서 전체 graph를 처음 build |
| `noop` | 같은 source와 같은 build directory에서 재실행 |
| `incremental` | 하나의 C source를 바꾼 뒤 같은 build directory에서 재실행 |

출력 포맷은 사람이 읽을 수 있는 line protocol이다.

```txt
medium_project_gate scheduler host_jobs=10
medium_project_gate scheduler default_jobs=10 ready_width=40 async_final_actions=40 trace_elapsed_ms=257
medium_project_gate backend=stella phase=clean elapsed_ms=123
medium_project_gate backend=stella phase=noop elapsed_ms=42
medium_project_gate backend=stella phase=incremental elapsed_ms=51
medium_project_gate backend=stella-jobs jobs=10 phase=clean elapsed_ms=118
medium_project_gate staticlib_argv_parity=ok target=//sys/kern/mm:kernel_mm
medium_project_gate backend=ninja phase=clean elapsed_ms=100
medium_project_gate backend=ninja phase=noop elapsed_ms=30
medium_project_gate backend=ninja phase=incremental elapsed_ms=36
medium_project_gate compare phase=clean stella_ms=123 ninja_ms=100 ratio_x100=200 slack_ms=250
medium_project_gate status=ok perf_issue_count=0 report_only=1
```

Ninja가 설치되어 있지 않으면 Ninja phase는 `skipped`로 기록한다. Stella phase는 항상
실행되어야 한다.

## Goals

- no-op build는 0.2초대에 접근해야 한다.
- clean build는 Ninja 대비 1.5-2배 이내를 목표로 한다.
- incremental build는 no-op에 가까운 overhead와 하나의 changed target rebuild만
  포함해야 한다.

## Latest Snapshot

Round Q137 local macOS arm64 대표 측정값:

```txt
medium_project_gate target_count=47 min_targets=40
medium_project_gate scheduler host_jobs=10
medium_project_gate scheduler default_jobs=10 ready_width=40 async_final_actions=40 trace_elapsed_ms=257
medium_project_gate backend=stella phase=clean elapsed_ms=247
medium_project_gate backend=stella phase=noop elapsed_ms=67
medium_project_gate backend=stella phase=incremental elapsed_ms=89
medium_project_gate backend=stella-jobs jobs=10 phase=clean elapsed_ms=237
medium_project_gate backend=stella-jobs jobs=10 phase=noop elapsed_ms=68
medium_project_gate backend=stella-jobs jobs=10 phase=incremental elapsed_ms=88
medium_project_gate staticlib_argv_parity=ok target=//sys/kern/mm:kernel_mm
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

Stella no-op과 incremental은 이 corpus에서 Ninja급 latency를 보인다. Q121은 compact
`state.db` dirty-check path를 추가해 JSON state parse를 no-op/incremental hot path에서
피했고, Q123은 `deps.db` compact dependency state를 추가해 depfile-discovered header
list 재파싱을 줄인다. Q124는 `actions.qsa`를 실행 가능한 lowered action plan으로 확장해
cache hit 때 compile/archive/link argv와 description materialization을 건너뛴다. Q125는
clean build에서 state/deps/action metadata write path를 buffered write로 정리하고,
build directory 내부 generated object/archive input은 content hash 대신 size/mtime 기반
metadata key로 다룬다. Q129는 compile/archive/link/custom generated action start path에
POSIX spawn runner를 추가했다. macOS와 Linux/glibc는 `posix_spawn` fast path를 사용하고,
unsupported platform이나 spawn setup failure는 기존 fork/exec path로 fallback한다. Q130은
compile/custom action wait loop에서 fixed sleep pause를 제거하고 stdout/stderr pipe readiness를
POSIX `poll()`로 기다린다. Q131은 successful/cache-hit action log를 lazy materialization으로
전환해 clean build metadata file write 수를 줄인다. Q132는 `state.db`를 Stella
dirty-check의 canonical fast path로 명확화하고, 사람이 읽는 `state/actions.json` dump를
`QSTAR_DEBUG_STATE_DUMPS=1` opt-in으로 내려 fast path에서 JSON debug export write를
제거했다. Q133-Q136은 macOS default jobs 감지, staticlib dependency archive nesting 제거,
compile dependency edge 완화, archive/link final action async scheduling을 더했다.

Q137 대표 측정에서는 Stella clean이 Ninja 대비 2배 이내 목표를 넘어 1.5배 이내에 들어왔다.
다만 timing은 host CPU, filesystem cache, compiler warm state에 흔들리므로, 이 수치를 stable
성능 보장으로 선언하지 않는다. 현재 gate는 default jobs가 host CPU count로 잡히는지,
초기 ready queue 폭이 충분한지, archive/link final action이 async schedule에 올라가는지,
staticlib archive argv가 dependency `.a`를 다시 넣지 않는지를 hard check한다. Timing ratio는
release 판단용 report-only로 유지한다.

남은 병목은 더 큰 corpus에서의 compiler process count, generated/run action의 동기 경로,
remaining graph/metadata summary write, host별 process runner 편차다. 다음 목표는 medium
gate에서 Stella clean을 Ninja 대비 1.5배 이내에 더 안정적으로 유지하고, 더 큰 synthetic
corpus에서도 같은 경향이 나오는지 확인하는 것이다.

Timing은 host CPU, filesystem cache, compiler, terminal load에 영향을 받는다. 그래서
Round Q92 기준 timing threshold는 기본적으로 report-only다. 구조적 실패, graph 실패,
compile database 누락, command 실패는 계속 hard fail이다.

## Environment Knobs

| Variable | Default | 의미 |
| --- | ---: | --- |
| `QSTAR_MEDIUM_PERF_REPORT_ONLY` | `1` | `1`이면 timing threshold 초과를 warning으로 기록 |
| `QSTAR_MEDIUM_CLEAN_MAX_MS` | `120000` | clean phase 목표 시간 |
| `QSTAR_MEDIUM_NOOP_MAX_MS` | `300` | no-op phase 목표 시간 |
| `QSTAR_MEDIUM_INCREMENTAL_MAX_MS` | `1000` | incremental phase 목표 시간 |
| `QSTAR_MEDIUM_STELLA_TO_NINJA_X100` | `200` | Stella/Ninja 허용 비율. `200`은 2.0배 |
| `QSTAR_MEDIUM_RATIO_SLACK_MS` | `250` | 작은 project에서 ratio noise를 흡수하는 절대 slack |
| `QSTAR_MEDIUM_MIN_TARGETS` | `40` | dynamic medium corpus 최소 target 수 |

Release 후보에서 timing을 hard gate로 승격하려면 다음처럼 실행한다.

```sh
QSTAR_MEDIUM_PERF_REPORT_ONLY=0 make qstar-medium-project-readiness-tests
```

## Perf Summary Tool

Q139부터 raw line protocol을 직접 눈으로 읽지 않고 `tools/perf-summary.sh`로 요약한다.
이 도구는 `medium_project_gate`와 `large_project_gate`를 모두 읽고, 동일한
gate/mode/backend/phase sample의 `min`, `median`, `max`와 Stella/Ninja ratio를 출력한다.

```sh
QSTAR_TEST_QSTAR=./build/bin/qstar sh tests/medium-project-performance.sh \
  > /tmp/qstar-medium.perf
tools/perf-summary.sh /tmp/qstar-medium.perf
```

대표 출력:

```txt
perf_summary sample gate=medium mode=medium backend=stella phase=clean count=3 min_ms=237 median_ms=247 max_ms=260
perf_summary ratio gate=medium mode=medium backend=stella phase=clean backend_median_ms=247 ninja_median_ms=251 ratio_x100=98 threshold_x100=200 slack_ms=250 status=ok
perf_summary status=ok sample_count=9 ratio_count=6 warning_count=0 hard=0 threshold_x100=200 slack_ms=250
```

Release note에 붙일 표는 markdown format으로 만든다.

```sh
tools/perf-summary.sh --format markdown --label "QStar v0.5 perf snapshot" \
  /tmp/qstar-medium.perf
```

3회 반복 측정도 같은 도구에서 지원한다. `--` 뒤 command는 shell string이 아니라 argv-vector로
실행한다. Environment override가 필요하면 `env`나 `sh -c`를 명시한다.

```sh
tools/perf-summary.sh --repeat 3 -- \
  env QSTAR_TEST_QSTAR=./build/bin/qstar sh tests/medium-project-performance.sh
```

Threshold는 기본적으로 report-only다. `--hard`를 붙이면 ratio warning이 exit failure가 된다.
따라서 local/release note 수집에는 기본 mode를 쓰고, CI에서 timing을 gate로 승격할 때만
`--hard`를 사용한다.

```sh
tools/perf-summary.sh --ratio-x100 200 --slack-ms 250 --hard /tmp/qstar-medium.perf
```

Makefile target도 제공한다.

```sh
make qstar-perf-summary-tests
```

## Static Fixture

사용자가 직접 Stella build를 반복 실행할 수 있도록 정적 fixture도 유지한다.

```sh
./build/bin/qstar --file tests/corpus/medium/qstar.lua build //:all
./build/bin/qstar --file tests/corpus/medium/qstar.lua build //:all
```

이 fixture는 `qstar.group`, `qstar.config`, 여러 `qstar.staticlib`, 하나의 executable을
포함한다. 성능 비교의 정본은 dynamic gate이고, 정적 fixture는 authoring과 manual smoke를
위한 안정된 입력이다.

## Large Synthetic Corpus Gate

Q138부터 large synthetic corpus gate를 별도로 둔다. Medium gate는 release smoke에
가깝고, large gate는 Stella/Ninja scheduler scaling을 보기 위한 report-only 성능 입력이다.

```sh
QSTAR_TEST_QSTAR=./build/bin/qstar sh tests/large-project-performance.sh
```

Makefile target도 제공한다.

```sh
make qstar-large-project-performance-tests
```

기본 mode는 200 target과 500 target이다.

```sh
QSTAR_LARGE_PROJECT_TARGETS="200 500" sh tests/large-project-performance.sh
```

빠른 local 확인이 필요하면 한 mode만 실행할 수 있다.

```sh
QSTAR_LARGE_PROJECT_TARGETS=200 QSTAR_TEST_QSTAR=./build/bin/qstar \
  sh tests/large-project-performance.sh
```

Large corpus shape:

- `qstar.staticlib` fanout target을 mode별로 생성한다.
- mode별로 하나 이상의 `qstar.executable` link shard가 staticlib를 나누어 dependency로 가진다.
- `qstar.group "all"`이 top-level aggregate다.
- 여러 `qstar.custom_target`이 fake external compiler를 호출해
  `qstar.output(..., {format = "object"})` object artifact를 만든다.
- 생성된 object artifact는 staticlib source로 소비되고, executable shard는 해당 staticlib를
  link dependency로 받는다.
- Stella default, Stella explicit `--jobs`, Ninja의 clean/no-op/incremental 시간을 모두
  기록한다.
- 각 backend는 동일한 synthetic source tree를 별도 임시 root에 생성해 측정한다. Project-level
  `generated_dir`가 backend 간에 공유되어 clean 비교가 흐려지는 일을 피하기 위해서다.

Large gate line protocol은 `large_project_gate` prefix를 쓴다.

```txt
large_project_gate mode=200 target_count=200 generated_actions=4 host_jobs=10
large_project_gate mode=200 backend=stella phase=clean elapsed_ms=1234
large_project_gate mode=200 backend=stella-jobs jobs=10 phase=clean elapsed_ms=1200
large_project_gate mode=200 backend=ninja phase=clean elapsed_ms=900
large_project_gate mode=200 compare phase=clean stella_ms=1234 ninja_ms=900 ratio_x100=200 slack_ms=500
large_project_gate status=ok perf_issue_count=0 report_only=1 modes="200 500"
```

Large gate의 timing threshold는 기본적으로 report-only다. Graph failure, build failure,
compile database 누락, generated object bridge 누락, Ninja root `.ninja_log`/`.ninja_deps`
오염은 hard fail이다.

Large gate가 보는 질문은 medium gate와 다르다.

| Gate | 목적 |
| --- | --- |
| Medium | release smoke와 beta readiness 대표값 |
| Large | target/action 수가 늘어날 때 scheduler scaling, link sharding, metadata overhead 관찰 |

Large gate 결과가 안정화되기 전까지는 release note에 숫자를 넣더라도 "representative
local run"으로만 표기한다. Stable 성능 보장으로 쓰지 않는다.

Round Q138 local macOS arm64 대표 측정값:

```txt
large_project_gate mode=200 target_count=200 generated_actions=4 host_jobs=10
large_project_gate mode=200 backend=stella phase=clean elapsed_ms=1115
large_project_gate mode=200 backend=stella phase=noop elapsed_ms=77
large_project_gate mode=200 backend=stella phase=incremental elapsed_ms=115
large_project_gate mode=200 backend=stella-jobs jobs=10 phase=clean elapsed_ms=1123
large_project_gate mode=200 backend=stella-jobs jobs=10 phase=noop elapsed_ms=77
large_project_gate mode=200 backend=stella-jobs jobs=10 phase=incremental elapsed_ms=116
large_project_gate mode=200 backend=ninja phase=clean elapsed_ms=2202
large_project_gate mode=200 backend=ninja phase=noop elapsed_ms=96
large_project_gate mode=200 backend=ninja phase=incremental elapsed_ms=146
large_project_gate mode=500 target_count=500 generated_actions=4 host_jobs=10
large_project_gate mode=500 backend=stella phase=clean elapsed_ms=2284
large_project_gate mode=500 backend=stella phase=noop elapsed_ms=98
large_project_gate mode=500 backend=stella phase=incremental elapsed_ms=139
large_project_gate mode=500 backend=stella-jobs jobs=10 phase=clean elapsed_ms=5171
large_project_gate mode=500 backend=stella-jobs jobs=10 phase=noop elapsed_ms=103
large_project_gate mode=500 backend=stella-jobs jobs=10 phase=incremental elapsed_ms=150
large_project_gate mode=500 backend=ninja phase=clean elapsed_ms=2545
large_project_gate mode=500 backend=ninja phase=noop elapsed_ms=159
large_project_gate mode=500 backend=ninja phase=incremental elapsed_ms=204
large_project_gate status=ok perf_issue_count=0 report_only=1 modes="200 500"
```

해석:

- 200 target mode에서는 Stella default와 explicit jobs clean이 Ninja보다 빠르게 측정됐다.
- 500 target mode에서는 Stella default clean이 Ninja보다 빠르게 측정됐고, explicit jobs clean은
  이번 run에서 느리게 튀었지만 report-only ratio 범위 안에 있었다.
- no-op과 incremental은 200/500 mode 모두 Ninja급 또는 그보다 빠른 범위에 들어왔다.
- 이 결과는 large target count에서 Q137의 scheduler 개선이 무너지지 않는다는 첫 증거다.
