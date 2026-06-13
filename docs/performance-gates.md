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
medium_project_gate backend=stella phase=clean elapsed_ms=123
medium_project_gate backend=stella phase=noop elapsed_ms=42
medium_project_gate backend=stella phase=incremental elapsed_ms=51
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

Round Q125 local macOS arm64 대표 측정값:

```txt
medium_project_gate target_count=47 min_targets=40
medium_project_gate backend=stella phase=clean elapsed_ms=746
medium_project_gate backend=stella phase=noop elapsed_ms=69
medium_project_gate backend=stella phase=incremental elapsed_ms=91
medium_project_gate backend=ninja phase=clean elapsed_ms=254
medium_project_gate backend=ninja phase=noop elapsed_ms=73
medium_project_gate backend=ninja phase=incremental elapsed_ms=105
medium_project_gate compare phase=clean stella_ms=746 ninja_ms=254 ratio_x100=200 slack_ms=250
medium_project_gate compare phase=noop stella_ms=69 ninja_ms=73 ratio_x100=200 slack_ms=250
medium_project_gate compare phase=incremental stella_ms=91 ninja_ms=105 ratio_x100=200 slack_ms=250
medium_project_gate status=ok perf_issue_count=0 report_only=1
```

Stella no-op과 incremental은 이 corpus에서 Ninja급 latency를 보인다. Q121은 compact
`state.db` dirty-check path를 추가해 JSON state parse를 no-op/incremental hot path에서
피했고, Q123은 `deps.db` compact dependency state를 추가해 depfile-discovered header
list 재파싱을 줄인다. Q124는 `actions.qsa`를 실행 가능한 lowered action plan으로 확장해
cache hit 때 compile/archive/link argv와 description materialization을 건너뛴다. Q125는
clean build에서 state/deps/action metadata write path를 buffered write로 정리하고,
build directory 내부 generated object/archive input은 content hash 대신 size/mtime 기반
metadata key로 다룬다.

Clean build는 Q124 대표값 808ms에서 Q125 대표값 746ms로 내려갔지만, 목표 범위였던
500-650ms에는 아직 닿지 못했다. 현재 remaining gap은 successful action log/replay write,
process pipe wait/drain, scheduler/process boundary 쪽에 남아 있다. 다만 no-op과
incremental은 계속 Ninja급 latency를 유지하고, medium corpus에서는 1초 미만을 유지한다.

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
