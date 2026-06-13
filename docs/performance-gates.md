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

## Static Fixture

사용자가 직접 Stella build를 반복 실행할 수 있도록 정적 fixture도 유지한다.

```sh
./build/bin/qstar --file tests/corpus/medium/qstar.lua build //:all
./build/bin/qstar --file tests/corpus/medium/qstar.lua build //:all
```

이 fixture는 `qstar.group`, `qstar.config`, 여러 `qstar.staticlib`, 하나의 executable을
포함한다. 성능 비교의 정본은 dynamic gate이고, 정적 fixture는 authoring과 manual smoke를
위한 안정된 입력이다.
