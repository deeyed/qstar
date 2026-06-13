# Stella Ninja Architecture Profiling Report

이 문서는 Round Q118에서 Ninja의 executor 구조를 분석하고, Stella executor를 Ninja급
체감 속도에 더 가깝게 만들기 위한 성능 개선 방향을 고정한 임시 보고서다.

```txt
status: q118 ninja architecture profiling report
date: 2026-06-13
ninja clone: /tmp/qstar-ninja-profile
ninja commit: 5a7fe11
qstar version: 0.5.1-beta.1
scope: architecture profiling, no code copied from Ninja
```

## 결론

Stella의 no-op과 incremental path는 medium corpus에서 이미 Ninja와 같은 체감 범위에
있다. 문제는 clean build다. Q118 재측정에서 Stella clean은 792-991ms, Ninja clean은
261-294ms였다. 즉 작은 medium corpus에서도 clean path는 Stella가 Ninja보다 대략
2.7-3.8배 느리게 흔들린다.

Ninja를 분석한 결론은 단순하다. Ninja는 build 시점에 이미 낮은 수준으로 lowering된
graph를 읽고, dirty check와 ready queue 실행만 한다. Stella는 QStar의 고수준 Lua DSL,
config merge, diagnostic-friendly graph validation, action materialization, JSON state,
action log/replay surface를 매 build invocation에서 함께 처리한다. 따라서 Stella가
Ninja에 가까워지려면 authoring surface를 줄이는 것이 아니라, 실행 직전 단계에
Stella 전용 lowered plan cache를 둬야 한다.

## Q118 Timing

명령:

```sh
sh tests/medium-project-performance.sh
```

측정 환경은 local macOS arm64이며, timing은 filesystem cache와 system load에 영향을
받는다. 그래서 수치는 절대값보다 추세를 본다.

| Run | Stella clean | Stella no-op | Stella incremental | Ninja clean | Ninja no-op | Ninja incremental |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 792ms | 105ms | 123ms | 294ms | 83ms | 107ms |
| 2 | 846ms | 82ms | 105ms | 291ms | 87ms | 111ms |
| 3 | 991ms | 90ms | 97ms | 261ms | 78ms | 104ms |

해석:

- no-op은 Stella 82-105ms, Ninja 78-87ms로 충분히 가깝다.
- incremental은 Stella 97-123ms, Ninja 104-111ms로 같은 체감권이다.
- clean은 Stella 792-991ms, Ninja 261-294ms로 격차가 명확하다.
- 따라서 다음 최적화는 process execution 자체보다 clean build의 graph/action/material
  hot path에 집중해야 한다.

## Ninja Fast Path

분석 대상 파일:

- `src/build.cc`
- `src/graph.cc`
- `src/deps_log.cc`
- `src/build_log.cc`
- `src/disk_interface.cc`
- `src/subprocess-posix.cc`
- `src/subprocess-win32.cc`

Ninja의 핵심 executor 구현은 위 파일 기준 약 4천 LOC다. 역할 분리가 선명하고, build
시점에는 authoring 언어 평가가 없다.

### Plan And Ready Queue

`src/build.cc`의 `Plan`은 wanted edge와 ready queue를 매우 단순하게 관리한다. 이미
처리한 edge는 다시 schedule하지 않고, phony edge는 command count에서 제외한다.
`Builder::Build` loop는 가능한 만큼 command를 시작하고, 완료된 command를 reap한 뒤,
다시 ready edge를 채우는 반복 구조다.

QStar에 적용할 점:

- Stella scheduler도 action DAG를 이미 갖고 있으므로, 다음 단계는 ready queue에 올라가기
  전 action materialization을 더 줄이는 것이다.
- `group`/no-op target은 이미 progress action에서 제외하지만, plan cache 단계에서는 아예
  execution node를 만들지 않는 방향으로 더 줄일 수 있다.
- action description, log path, replay path 같은 user-facing 문자열은 ready queue 구성
  시점이 아니라 실제 출력/실행 직전에 lazy materialize한다.

### Dirty Scan

`src/graph.cc`의 `DependencyScan::RecomputeDirty`는 node/edge dirty state를 재귀적으로
계산한다. 핵심 판단은 output missing, output older than input, command hash changed,
depfile/deps log missing 정도로 제한된다.

QStar에 적용할 점:

- Stella도 action key와 output existence로 cache-hit을 판단하지만, key material을 매번
  문자열로 조립한다.
- lowered plan cache에는 이미 계산된 action id, owner, output, argv digest, profile
  digest, input list digest를 넣고, invocation hot path에서는 파일 mtime/hash와 compact
  state만 비교하도록 한다.
- `why-rebuild`와 `--explain-cache`용 상세 reason은 일반 build path에서 항상 만들지 말고
  필요할 때만 계산한다.

### Build Log And Deps Log

`src/build_log.cc`는 command hash, start/end time, output mtime을 output path 기준으로
저장한다. 파일은 첫 write 때 lazy open한다. `src/deps_log.cc`는 discovered deps를 binary
record로 저장하고, 새 정보가 없으면 write하지 않는다.

QStar에 적용할 점:

- 현재 Stella `state/actions.json`은 사람이 읽기 좋지만, clean/no-op hot path에는 무겁다.
- 1차 개선은 JSON state를 유지하되, 내부 fast state index 파일을 추가하는 것이다.
- 2차 개선은 `build/qstar/stella/state.db` 같은 compact state DB로 action id -> digest,
  output, mtime, depfile digest를 바로 읽게 하는 것이다.
- action log/replay는 QStar의 장점이므로 없애지 않는다. 대신 실행 성공 path에서는 batch
  write를 유지하고, no-op path에서는 log write를 생략하는 현재 방향을 더 강화한다.
- Q125 이후 source input은 content digest를 유지하지만, `build_dir` 내부 generated
  object/archive input은 content를 다시 읽지 않고 size/mtime metadata를 key material로
  사용한다. Clean build에서 archive/link가 방금 생성한 `.o`를 다시 읽는 비용을 줄이기
  위한 Stella-specific fast path다.

### Disk Interface

`src/disk_interface.cc`는 stat path를 좁게 유지한다. Windows에서는 directory stat cache를
사용하고, POSIX path에서는 단순 stat을 빠르게 수행한다. Ninja dirty scan에서 stat은 node가
필요할 때만 호출된다.

QStar에 적용할 점:

- Stella는 input/output key 계산에서 path formatting과 existence/hash check를 반복할 수
  있다.
- build-local stat memoization을 action key 계산 전체에 적용한다.
- path normalization과 `full_path_under_*` 결과를 action materialization 단계에서 재사용할
  수 있게 한다.
- clean build에서는 output existence check와 depfile refresh를 분리해, 컴파일 전 필요한
  최소 stat만 수행한다.

### Subprocess Wait

`src/subprocess-posix.cc`는 `ppoll` 또는 `pselect` 기반으로 running subprocess의 pipe와
jobserver token을 기다린다. Stella는 아직 일부 경로에서 `waitpid(..., WNOHANG)` polling과
짧은 pause를 사용한다.

QStar에 적용할 점:

- Stella의 process wait loop를 poll/select 기반으로 바꿔 short action latency와 CPU wakeup을
  줄인다.
- stdout/stderr line coloring은 유지하되, pipe drain과 wait를 하나의 event loop로 묶는다.
- single action path와 scheduler path가 같은 process runner를 공유하게 한다.

## Stella Current Hot Spots

QStar source 기준 관찰:

- `src/main.c`는 CLI option 처리 뒤 `qstar_lua_eval_file`을 실행하고, 그 후 subcommand를
  실행한다. 즉 현재 `qstar build`는 authoring input이 바뀌지 않아도 Lua eval과 graph
  validation을 통과한다.
- `src/plan.c`는 target closure를 매번 계산하고 dependency label을 순회한다.
- `src/executor.c`의 state reader/writer는 line-oriented JSON state를 읽고 쓴다.
- `src/executor.c`의 action state entry는 id/key/output/status/kind와 여러 digest 문자열을
  동적 할당한다.
- `src/executor.c`는 compile 성공 후 depfile-discovered input을 읽고 action material을
  갱신한다. 이 구조는 정확성에는 좋지만, clean path에서는 per-action overhead가 생긴다.
- `src/executor.c`의 process wait loop는 아직 Ninja식 event loop보다 단순 polling 성격이
  강하다.

이것들은 모두 기능적으로는 옳다. 하지만 Ninja급 clean build를 목표로 할 때는 hot path에서
줄여야 할 비용이다.

## Required Design Shift

Stella가 장기적으로 Ninja에 가까워지려면 다음 구조가 필요하다.

```txt
qstar.lua / .qst / .qsm
  -> Lua eval
  -> QStar Graph IR
  -> Stella Plan IR
  -> build/qstar/stella/plan cache
  -> Stella executor dirty check and scheduler
```

사용자-facing command는 그대로 둔다.

```sh
qstar build //:app
```

하지만 내부적으로는 다음 순서가 되어야 한다.

1. `qstar.lua`, imported `.qst`, `.qsm`, QStar version, selected profile, `-B`, `-G`,
   package aliases, relevant environment fingerprint를 확인한다.
2. fingerprint가 같으면 Lua eval과 Graph IR validation을 건너뛰고 lowered Stella Plan IR을
   읽는다.
3. action dirty check와 scheduler만 실행한다.
4. fingerprint가 다르면 Lua eval부터 다시 하고 plan cache를 갱신한다.

이 구조는 QStar의 Lua DSL 장점을 버리는 것이 아니라, authoring 단계와 execution 단계를
분리하는 것이다.

## Proposed Internal Files

내부 파일명은 user-facing DSL 확장자가 아니므로 안정 API로 공개하지 않는다.

```txt
build/qstar/stella/
  graph.qsg        # lowered graph summary, internal
  actions.qsa      # lowered action plan, internal
  deps.db          # depfile/discovered dependency db, internal
  state.db         # compact action state, internal
  log              # compact scheduler/action timing log, internal
```

초기 구현은 text/JSON으로 시작해도 된다. 단, 설계 목표는 binary 또는 compact line format으로
옮기는 것이다. 중요한 것은 형식이 아니라 "Lua eval과 action materialization을 매번 하지
않는 것"이다.

## No Code Copy Rule

Ninja source는 architecture reference로만 사용한다. QStar에는 Ninja 코드를 복사하지 않는다.
적용 가능한 것은 다음 수준이다.

- ready queue와 dirty scan의 구조적 아이디어
- lazy log open과 compact deps/state DB 아이디어
- event-driven subprocess wait 아이디어
- phony/no-op edge를 execution action에서 제외하는 원칙

구현은 QStar 코드 스타일과 Apache-2.0 release policy에 맞춰 새로 작성한다.

## Source Anchors

Q118 분석에서 확인한 주요 source anchor:

| 영역 | 파일/라인 |
| --- | --- |
| Ninja plan/ready queue | `src/build.cc:95-199` |
| Ninja main build loop | `src/build.cc:699-820` |
| Ninja dirty traversal | `src/graph.cc:49-185` |
| Ninja output dirty rule | `src/graph.cc:271-365` |
| Ninja build log lazy write | `src/build_log.cc:77-140` |
| Ninja deps log compact write | `src/deps_log.cc:51-145` |
| Ninja deps log load | `src/deps_log.cc:154-175` |
| Ninja disk stat path | `src/disk_interface.cc:215-270` |
| Ninja POSIX subprocess wait | `src/subprocess-posix.cc:312-485` |
| QStar CLI Lua eval before command dispatch | `src/main.c:913-965` |
| QStar target closure walk | `src/plan.c:170-275` |
| Stella JSON state load/write | `src/executor.c:1811-2145` |
| Stella single action runner | `src/executor.c:2665-2882` |
| Stella depfile refresh | `src/executor.c:3989-4065` |
| Stella compile batch scheduler | `src/executor.c:4503-4665` |

## Next Rounds

### Q119: Stella Plan Cache Spec

- Plan cache fingerprint 계약 작성
- invalidation 조건 정의
- internal file layout 문서화
- `qstar explain`/`doctor`에서 cache status를 보여줄지 결정
- 결과 문서: `docs/perf/stella-plan-cache-design.md`

### Q120: Stella Plan Cache MVP

- unchanged authoring input이면 Lua eval/Graph IR 재구성 skip
- validated Graph IR와 lowered action summary를 `build/qstar/stella/`에서 로드
- no-op/incremental timing 재측정
- 결과: no-op 103ms, incremental 136ms로 Ninja no-op 107ms, incremental 156ms와 같은
  체감권에 들어왔다.

### Q121: Compact Dirty State

- `build/qstar/state/actions.json` 옆에 fast-path `build/qstar/state/state.db` 추가
- Stella build start에서 compact DB를 먼저 읽고, 없거나 stale이면 JSON state로 fallback
- action id -> output, command digest, input digest, depfile digest lookup의 JSON parse overhead 제거
- JSON state는 debugging/export surface로 유지
- `--schedule-trace`에서는 `dirty_state_db status=hit|miss`로 compact path 사용 여부 확인 가능
- 대표 측정: Stella no-op 70ms, incremental 111ms로 Ninja no-op 80ms, incremental 120ms와
  같은 체감권을 유지했다. Clean은 745ms 대 Ninja 304ms로 다음 병목은 process runner와
  dependency DB 쪽이다.

### Q122: Event-Driven Process Runner

- single action path와 scheduler path가 공유하는 process runner 추가
- `waitpid(WNOHANG)` polling을 poll/select 계열 event wait로 교체
- warning/error coloring과 action log/replay는 유지

### Q123: Clean Build Hard Gate Trial

- medium corpus에서 clean ratio gate를 report-only에서 hard-fail 후보로 전환하는 실험
- 목표: Stella clean이 Ninja clean 대비 2배 이내, 가능하면 1.5배 근접
- no-op/incremental은 현재 수준 유지

## Q118 Verdict

Stella를 Ninja급으로 만들 수 있는 가장 큰 lever는 더 많은 micro-optimization이 아니라
lowered plan cache다. QStar는 고수준 authoring DSL을 유지하고, Stella는 build execution
직전에 Ninja처럼 낮은 수준의 graph만 다루도록 가야 한다. Q118 이후 성능 작업은 새 기능보다
이 방향에 집중하는 것이 맞다.
