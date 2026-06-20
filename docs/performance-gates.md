# QStar Performance Gates

QStar는 Stella executor, experimental Stella daemon path, Ninja backend를 같은 medium/large
project shape에서 비교해 executor 품질을 숫자로 관리한다. 이 문서는 release gate의 개발자용
계약이다. 사용자 문서는 `wiki/reference/performance-gates.md`를 함께 갱신한다.

## Medium Corpus Gate

기본 gate는 다음 명령으로 실행한다.

```sh
make qstar-medium-project-readiness-tests
```

Generic DSL hard cut 이후 backend/performance release seal은 self-host,
Stella/Ninja backend parity, Linux validation, medium performance summary를 함께 묶는다.

```sh
make qstar-generic-dsl-backend-parity-tests
```

이 target은 `tools/perf-summary.sh --hard`를 사용해 Stella/Ninja ratio가
기본 hard threshold인 `2.5x + 500ms`를 넘으면 실패한다. 구조적 backend failure는
항상 hard fail이다.

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

Ninja가 설치되어 있지 않으면 Ninja phase는 `skipped`로 기록한다. Stella phase는 항상
실행되어야 한다. Daemon socket bind가 sandbox나 host policy 때문에 불가능하면 daemon phase는
`elapsed_ms=skipped reason=socket-bind-not-permitted`로 기록한다.

## Goals

- no-op build는 0.2초대에 접근해야 한다.
- clean build는 Ninja 대비 1.5-2배 이내를 목표로 한다.
- incremental build는 no-op에 가까운 overhead와 하나의 changed target rebuild만
  포함해야 한다.

## Latest Snapshot

Round Q250 local macOS arm64 repeat-3 대표 측정값은
`docs/perf/q250-v0.8-backend-performance-refresh.md`에 보존한다. 이 refresh는 GLP, Windows
beta path, generic command workflow, built-in install hard cut, daemon beta boundary hardening
이후 medium/large corpus와 Stella daemon을 다시 분리해서 측정한 값이다.

Medium corpus, daemon excluded:

| Backend | Clean | No-op | Incremental | Ninja ratio clean/no-op/incremental |
| --- | ---: | ---: | ---: | --- |
| Stella | 264ms | 72ms | 91ms | 1.01x / 0.97x / 0.91x |
| Stella explicit jobs | 290ms | 66ms | 90ms | 1.11x / 0.89x / 0.90x |
| Ninja | 262ms | 74ms | 100ms | baseline |

Medium corpus, daemon included:

| Backend | Clean | No-op | Incremental | Ninja ratio clean/no-op/incremental |
| --- | ---: | ---: | ---: | --- |
| Stella daemon | 259ms | 68ms | 89ms | 0.99x / 0.92x / 0.89x |
| Ninja | 262ms | 74ms | 100ms | baseline |

Large corpus, daemon excluded:

| Mode | Backend | Clean | No-op | Incremental | Ninja ratio clean/no-op/incremental |
| --- | --- | ---: | ---: | ---: | --- |
| 200 | Stella | 979ms | 81ms | 123ms | 0.86x / 0.77x / 0.79x |
| 200 | Stella explicit jobs | 999ms | 79ms | 121ms | 0.88x / 0.75x / 0.78x |
| 200 | Ninja | 1140ms | 105ms | 155ms | baseline |
| 500 | Stella | 4681ms | 100ms | 141ms | 1.20x / 0.65x / 0.68x |
| 500 | Stella explicit jobs | 4691ms | 98ms | 135ms | 1.20x / 0.64x / 0.65x |
| 500 | Ninja | 3893ms | 154ms | 207ms | baseline |

Large corpus, daemon included:

| Mode | Backend | Clean | No-op | Incremental | Ninja ratio clean/no-op/incremental |
| --- | --- | ---: | ---: | ---: | --- |
| 200 | Stella daemon | 1085ms | 84ms | 120ms | 0.95x / 0.80x / 0.77x |
| 200 | Ninja | 1140ms | 105ms | 155ms | baseline |
| 500 | Stella daemon | 2599ms | 108ms | 149ms | 0.67x / 0.70x / 0.72x |
| 500 | Ninja | 3893ms | 154ms | 207ms | baseline |

Old/new comparison:

| Snapshot | Stella clean | Stella no-op | Stella incremental | Ninja clean | Ninja no-op | Ninja incremental |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Q132 representative | 991ms | 79ms | 100ms | 276ms | 89ms | 107ms |
| Q137 default Stella | 247ms | 67ms | 89ms | 251ms | 73ms | 97ms |
| Q137 explicit jobs | 237ms | 68ms | 88ms | 251ms | 73ms | 97ms |
| Q233 medium Stella | 244ms | 68ms | 96ms | 254ms | 74ms | 116ms |
| Q233 medium daemon | 289ms | 68ms | 88ms | 254ms | 74ms | 116ms |
| Q250 medium Stella | 264ms | 72ms | 91ms | 262ms | 74ms | 100ms |
| Q250 medium daemon | 259ms | 68ms | 89ms | 262ms | 74ms | 100ms |

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
Q140은 external generated action과 run action도 prepared-action scheduler path에 올렸고,
Q141은 `state/graph.json`과 성공 `state/last-summary.json`을 debug/export opt-in으로 내려
일반 clean build의 metadata write를 더 줄였다.

Q137 대표 측정에서는 Stella clean이 Ninja 대비 2배 이내 목표를 넘어 1.5배 이내에 들어왔다.
Q233 대표 측정에서는 GLP/Windows work 이후에도 medium normal Stella가 Ninja와 같은 급이고,
large 200/500 target corpus에서는 normal Stella와 Stella daemon 모두 Ninja보다 빠르게 나왔다.
Q250 대표 측정에서는 medium과 large 200은 계속 같은 경향을 보였지만, large 500 clean에서는
normal Stella와 explicit-jobs Stella median이 Ninja보다 느려졌다. 같은 Q250 run에서 large 500
daemon clean, no-op, incremental은 Ninja보다 빠르게 측정됐다.
다만 timing은 host CPU, filesystem cache, compiler warm state에 흔들리므로, 이 수치를 stable
성능 보장으로 선언하지 않는다. 현재 gate는 default jobs가 host CPU count로 잡히는지,
초기 ready queue 폭이 충분한지, archive/link final action이 async schedule에 올라가는지,
staticlib archive argv가 dependency `.a`를 다시 넣지 않는지를 hard check한다. Timing ratio는
release 판단용 report-only로 유지한다.

남은 병목은 더 큰 corpus에서의 compiler process count, plan cache/store 비용, daemon
lifecycle/IPC overhead, host별 process runner 편차다. Real Rust/Zig GLP compiler corpus는
성능 숫자에 섞지 않고 `make qstar-real-glp-compiler-corpus-tests` correctness gate로
분리한다.

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
perf_summary sample gate=medium mode=medium backend=stella phase=clean count=3 min_ms=237 median_ms=247 max_ms=260 skipped_reason=
perf_summary sample gate=medium mode=medium backend=stella-daemon phase=noop count=3 min_ms=20 median_ms=22 max_ms=25 skipped_reason=
perf_summary sample gate=medium mode=medium backend=stella-daemon phase=clean count=1 min_ms=skipped median_ms=skipped max_ms=skipped skipped_reason=socket-bind-not-permitted
perf_summary ratio gate=medium mode=medium backend=stella phase=clean backend_median_ms=247 ninja_median_ms=251 ratio_x100=98 threshold_x100=200 slack_ms=250 warn_threshold_x100=200 warn_slack_ms=250 hard_threshold_x100=200 hard_slack_ms=250 status=ok
perf_summary ratio gate=medium mode=medium backend=stella-daemon phase=noop backend_median_ms=22 ninja_median_ms=73 ratio_x100=30 threshold_x100=200 slack_ms=250 warn_threshold_x100=200 warn_slack_ms=250 hard_threshold_x100=200 hard_slack_ms=250 status=ok
perf_summary status=ok sample_count=12 skipped_count=1 ratio_count=9 warning_count=0 hard_failure_count=0 hard=0 threshold_x100=200 slack_ms=250 hard_threshold_x100=200 hard_slack_ms=250
```

Release note에 붙일 표는 markdown format으로 만든다.

```sh
tools/perf-summary.sh --format markdown --label "QStar perf snapshot" \
  /tmp/qstar-medium.perf
```

Markdown sample table은 모든 host에서 같은 column을 쓴다. Daemon이 sandbox나 host policy 때문에
실행되지 않았으면 numeric cell을 비우지 않고 `Skipped reason` column에 이유를 남긴다.

```md
| Gate | Mode | Backend | Phase | Count | Min ms | Median ms | Max ms | Skipped reason |
| --- | --- | --- | --- | ---: | ---: | ---: | ---: | --- |
| medium | medium | stella | clean | 3 | 237 | 247 | 260 |  |
| medium | medium | stella-daemon | clean | 1 | - | - | - | socket-bind-not-permitted |

| Gate | Mode | Backend | Phase | Backend median ms | Ninja median ms | Ratio | Warn threshold | Hard threshold | Status |
| --- | --- | --- | --- | ---: | ---: | ---: | --- | --- | --- |
| medium | medium | stella | clean | 247 | 251 | 0.98x | 2.00x + 250ms | 2.00x + 250ms | ok |
```

3회 반복 측정도 같은 도구에서 지원한다. `--` 뒤 command는 shell string이 아니라 argv-vector로
실행한다. Environment override가 필요하면 `env`나 `sh -c`를 명시한다.

```sh
tools/perf-summary.sh --repeat 3 -- \
  env QSTAR_TEST_QSTAR=./build/bin/qstar sh tests/medium-project-performance.sh
```

Threshold는 warning threshold와 hard threshold로 나눈다. Warning threshold는 release note와
CI artifact에서 "주의가 필요한 수치"를 표시하는 report-only 기준이다. Hard threshold는
`--hard`와 함께 쓸 때 exit failure로 승격할 기준이다. `--ratio-x100`과 `--slack-ms`는
compatibility alias로 유지되며 warning threshold를 뜻한다.

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

Makefile target도 제공한다. `qstar-perf-summary-tests`는 도구 자체의 parser와 markdown
계약을 빠르게 검사하고, `qstar-performance-release-gate`는 medium/large raw log와 markdown
summary를 release artifact 형태로 만든다.

```sh
make qstar-perf-summary-tests

QSTAR_PERF_REPEAT=3 \
QSTAR_PERF_ARTIFACT_DIR=dist/perf \
make qstar-performance-release-gate
```

Release gate output:

```txt
dist/perf/medium-release-raw.txt
dist/perf/medium-release-summary.txt
dist/perf/medium-release-summary.md
dist/perf/large-release-raw.txt
dist/perf/large-release-summary.txt
dist/perf/large-release-summary.md
```

Q176 release gate format은 macOS local run과 Linux CI artifact가 같은 markdown table을
사용한다. Host/OS 차이는 label과 artifact name으로 표현하고, table column은 바꾸지 않는다.

| Snapshot | Host | Corpus | Repeat | Stella clean | Stella no-op | Stella incremental | Ninja clean | Ninja no-op | Ninja incremental | Daemon |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| Q166 | macOS arm64 | large 200 | 3 | 1054ms | 80ms | 137ms | 1220ms | 102ms | 154ms | measured |
| Q166 | macOS arm64 | large 500 | 3 | 2437ms | 101ms | 145ms | 2816ms | 144ms | 228ms | measured |
| Q169 | macOS arm64 | medium 47 | 1 | 236ms | 76ms | 91ms | 269ms | 74ms | 103ms | skipped: socket bind |
| Q176 dry run | macOS arm64 | medium 47 | 1 | 269ms | 68ms | 89ms | 252ms | 72ms | 97ms | skipped: socket bind |
| Q176 dry run | macOS arm64 | large 200 | 1 | 984ms | 72ms | 114ms | 1012ms | 92ms | 141ms | skipped: socket bind |
| Q250 | macOS arm64 | medium 47 | 3 | 264ms | 72ms | 91ms | 262ms | 74ms | 100ms | measured: 259/68/89ms |
| Q250 | macOS arm64 | large 200 | 3 | 979ms | 81ms | 123ms | 1140ms | 105ms | 155ms | measured: 1085/84/120ms |
| Q250 | macOS arm64 | large 500 | 3 | 4681ms | 100ms | 141ms | 3893ms | 154ms | 207ms | measured: 2599/108/149ms |

Q176 dry run은 `QSTAR_PERF_REPEAT=1 QSTAR_LARGE_PROJECT_TARGETS=200`으로 release artifact
format을 검증한 값이다. Public release note에는 repeat-3 또는 hosted CI artifact를 우선한다.

## Linux CI Performance Artifacts

Round Q142부터 `.github/workflows/linux-validation.yml`은 Ubuntu gcc/clang matrix에서
medium performance line protocol을 수집한다. 이 lane은 Linux에서 macOS와 비슷한 Stella/Ninja
경향이 재현되는지 보기 위한 validation input이며, timing threshold는 아직 report-only다.
Hard check는 다음 세 가지다.

- Stella medium build가 line protocol을 출력한다.
- Ninja backend clean phase가 같은 output에 포함된다.
- Stella scheduler trace가 Linux POSIX fast path를 보고한다.

```txt
medium_project_gate scheduler runner=posix_spawn event_wait=poll
medium_project_gate backend=ninja phase=clean elapsed_ms=...
```

CI artifact는 compiler lane별로 업로드한다.

```txt
dist/perf/linux-<compiler>-medium-perf.txt
dist/perf/linux-<compiler>-medium-summary.txt
dist/perf/linux-<compiler>-medium-summary.md
```

Large synthetic corpus는 더 오래 걸리므로 push/PR 기본 lane에 넣지 않는다. 우선
`workflow_dispatch` 전용 `large-performance-report` job에서 report-only artifact로 수집한다.

```txt
dist/perf/linux-gcc-large-perf.txt
dist/perf/linux-gcc-large-summary.txt
dist/perf/linux-gcc-large-summary.md
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
- Stella default, Stella explicit `--jobs`, Stella daemon, Ninja의 clean/no-op/incremental
  시간을 모두 기록한다.
- 각 backend는 동일한 synthetic source tree를 별도 임시 root에 생성해 측정한다. Project-level
  `generated_dir`가 backend 간에 공유되어 clean 비교가 흐려지는 일을 피하기 위해서다.

Large gate line protocol은 `large_project_gate` prefix를 쓴다.

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

Large gate의 timing threshold는 기본적으로 report-only다. Graph failure, build failure,
compile database 누락, generated object bridge 누락, Ninja root `.ninja_log`/`.ninja_deps`
오염은 hard fail이다. Daemon socket bind가 불가능한 sandbox에서는 daemon backend만 skipped로
기록하고 나머지 backend는 계속 측정한다.

Large gate가 보는 질문은 medium gate와 다르다.

| Gate | 목적 |
| --- | --- |
| Medium | release smoke와 beta readiness 대표값 |
| Large | target/action 수가 늘어날 때 scheduler scaling, link sharding, metadata overhead 관찰 |

Large gate 결과가 안정화되기 전까지는 release note에 숫자를 넣더라도 "representative
local run"으로만 표기한다. Stable 성능 보장으로 쓰지 않는다.

Round Q166 local macOS arm64 repeat-3 대표 측정값:

```txt
large_project_gate mode=200 target_count=200 generated_actions=4 host_jobs=10
perf_summary sample gate=large mode=200 backend=stella phase=clean count=3 min_ms=1006 median_ms=1054 max_ms=3553
perf_summary sample gate=large mode=200 backend=stella phase=noop count=3 min_ms=79 median_ms=80 max_ms=102
perf_summary sample gate=large mode=200 backend=stella phase=incremental count=3 min_ms=128 median_ms=137 max_ms=148
perf_summary sample gate=large mode=200 backend=stella-daemon phase=clean count=3 min_ms=1056 median_ms=1389 max_ms=1543
perf_summary sample gate=large mode=200 backend=stella-daemon phase=noop count=3 min_ms=81 median_ms=95 max_ms=97
perf_summary sample gate=large mode=200 backend=stella-daemon phase=incremental count=3 min_ms=125 median_ms=129 max_ms=178
perf_summary sample gate=large mode=200 backend=ninja phase=clean count=3 min_ms=1127 median_ms=1220 max_ms=3714
perf_summary sample gate=large mode=200 backend=ninja phase=noop count=3 min_ms=98 median_ms=102 max_ms=103
perf_summary sample gate=large mode=200 backend=ninja phase=incremental count=3 min_ms=153 median_ms=154 max_ms=157
large_project_gate mode=500 target_count=500 generated_actions=4 host_jobs=10
perf_summary sample gate=large mode=500 backend=stella phase=clean count=3 min_ms=2312 median_ms=2437 max_ms=4723
perf_summary sample gate=large mode=500 backend=stella phase=noop count=3 min_ms=98 median_ms=101 max_ms=104
perf_summary sample gate=large mode=500 backend=stella phase=incremental count=3 min_ms=140 median_ms=145 max_ms=160
perf_summary sample gate=large mode=500 backend=stella-daemon phase=clean count=3 min_ms=2540 median_ms=2601 max_ms=2726
perf_summary sample gate=large mode=500 backend=stella-daemon phase=noop count=3 min_ms=105 median_ms=107 max_ms=107
perf_summary sample gate=large mode=500 backend=stella-daemon phase=incremental count=3 min_ms=147 median_ms=151 max_ms=226
perf_summary sample gate=large mode=500 backend=ninja phase=clean count=3 min_ms=2504 median_ms=2816 max_ms=4311
perf_summary sample gate=large mode=500 backend=ninja phase=noop count=3 min_ms=142 median_ms=144 max_ms=369
perf_summary sample gate=large mode=500 backend=ninja phase=incremental count=3 min_ms=207 median_ms=228 max_ms=344
perf_summary status=ok sample_count=24 ratio_count=18 warning_count=0 hard=0 threshold_x100=200 slack_ms=250
```

해석:

- normal Stella는 200/500 target mode 모두 clean, no-op, incremental median에서 Ninja보다 빠르게 측정됐다.
- Stella daemon은 clean build에서 normal Stella보다 빠르지는 않지만, no-op/incremental은 같은 latency band에 있다.
- max 값이 넓게 흔들리므로 release note에는 median 중심으로만 인용한다.
- 이 결과는 large target count에서도 Stella scheduler scaling이 유지된다는 0.7 readiness 입력이다.
