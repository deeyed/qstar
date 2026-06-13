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
medium_project_gate backend=stella phase=clean elapsed_ms=123
medium_project_gate backend=stella phase=noop elapsed_ms=42
medium_project_gate backend=stella phase=incremental elapsed_ms=51
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

Round Q111 local macOS arm64 대표 측정값:

```txt
medium_project_gate target_count=47 min_targets=40
medium_project_gate backend=stella phase=clean elapsed_ms=773
medium_project_gate backend=stella phase=noop elapsed_ms=76
medium_project_gate backend=stella phase=incremental elapsed_ms=104
medium_project_gate backend=ninja phase=clean elapsed_ms=294
medium_project_gate backend=ninja phase=noop elapsed_ms=80
medium_project_gate backend=ninja phase=incremental elapsed_ms=97
medium_project_gate status=ok perf_issue_count=0 report_only=1
```

Stella no-op과 incremental은 이 corpus에서 Ninja급 latency를 보인다. Clean build는
Round Q111의 state lookup index, action key material reuse, lazy stdout/stderr log open
이후에도 raw ratio로는 Ninja의 2배 안쪽을 안정적으로 달성하지 못했다. 다만 slack을
포함한 report gate는 통과하며, medium corpus에서 1초 미만을 유지한다.

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
