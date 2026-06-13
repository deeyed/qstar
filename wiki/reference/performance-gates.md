# Performance Gates

QStar는 Stella executor 성능을 Ninja backend와 비교하는 medium project gate를 가진다.
목표는 “빠른 것 같다”가 아니라 clean, no-op, incremental build 시간을 line protocol로
남기고 release마다 추적하는 것이다.

## 실행

```sh
make qstar-medium-project-readiness-tests
```

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

Ninja가 없으면 Ninja phase는 `skipped`로 표시된다. Stella phase는 항상 실행된다.

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
dependency edge 완화, archive/link final action async scheduling을 더했다.

Q137 대표 측정에서는 Stella clean이 Ninja 대비 2배 이내 목표를 넘어 1.5배 이내에 들어왔다.
하지만 timing은 host CPU, filesystem cache, compiler warm state에 흔들리므로, 이 수치를
stable 성능 보장으로 선언하지 않는다. Gate는 default jobs, ready queue width,
async final action count, staticlib argv parity를 hard check하고, timing ratio는 report-only로
유지한다.

남은 병목은 더 큰 corpus에서의 compiler process count, generated/run action 동기 경로,
remaining graph/metadata summary write, host별 process runner 편차다. 다음 목표는 medium
gate에서 Stella clean을 Ninja 대비 1.5배 이내에 더 안정적으로 유지하고, 더 큰 synthetic
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
