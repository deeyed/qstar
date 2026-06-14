# Stella Lowered Plan Cache Design

이 문서는 Round Q119에서 Stella executor가 Lua eval 없이 재사용할 internal lowered plan
cache의 설계를 고정한다. 목표는 QStar의 Lua DSL authoring 장점은 유지하면서, 반복 build
invocation에서는 Ninja처럼 낮은 수준의 execution graph만 읽게 만드는 것이다.

```txt
status: q124 direct lowered action execution active
date: 2026-06-14
depends-on: docs/perf/stella-ninja-profile.md
scope: internal plan cache design and current behavior
implementation: graph cache plus executable compile/archive/link action plan active for Stella build
```

## Goal

현재 `qstar build`는 CLI parsing 이후 `qstar_lua_eval_file(...)`을 실행하고, Graph IR
validation과 action materialization을 거쳐 Stella executor로 들어간다. 이 경로는 정확하고
diagnostic-friendly하지만, clean build에서는 Ninja보다 많은 고수준 작업을 반복한다.

Stella plan cache의 목표:

- authoring input이 바뀌지 않았으면 Lua eval을 건너뛴다.
- Graph IR validation 결과와 action plan lowering 결과를 build directory에서 재사용한다.
- user-facing DSL, CLI, docs, diagnostics surface는 유지한다.
- stale cache로 잘못된 build를 하지 않는다. 성능보다 invalidation 정확성을 우선한다.

Non-goals:

- `qstar.lua`, `.qst`, `.qsm` 문법을 바꾸지 않는다.
- Ninja file format을 재사용하지 않는다.
- QStar dependency/package resolver를 만들지 않는다.
- Cale language-provider behavior를 강화하지 않는다. Cale source는 현 release line에서
  계속 Stella-only contract로 둔다.

## Pipeline

기존 경로:

```txt
CLI
  -> qstar_lua_eval_file
  -> Graph IR validation
  -> target closure
  -> action materialization
  -> dirty check
  -> scheduler
```

목표 경로:

```txt
CLI
  -> fingerprint probe
  -> plan cache hit?
      yes -> load Stella Plan IR
           -> dirty check
           -> scheduler
      no  -> qstar_lua_eval_file
           -> Graph IR validation
           -> target closure
           -> action materialization
           -> write Stella Plan IR
           -> dirty check
           -> scheduler
```

이 구조는 build command가 `qstar build //:app`인 사실을 바꾸지 않는다. 차이는 동일 입력의
반복 invocation에서 Lua/Graph 단계가 hot path에서 빠지는 것이다.

## Cache Directory

Stella cache는 build directory 아래에만 둔다. Project root나 package source tree에는 내부
cache file을 쓰지 않는다.

```txt
build/qstar/stella/
  manifest.json       # cache schema, qstar version, fingerprint summary
  inputs.json         # authoring input file fingerprints
  graph.qsg           # lowered graph summary, internal text v1
  actions.qsa         # executable lowered action plan, internal binary v2
  logs/
    plan-cache.log    # optional verbose/cache debug log
  tmp/
    *.tmp             # atomic write scratch files

build/qstar/state/
  actions.json        # opt-in debug/export action state
  state.db            # canonical compact dirty-check action state, internal binary v1
  deps.db             # compact discovered dependency DB, internal binary v1
```

초기 구현은 `manifest.json`, `inputs.json`, `graph.qsg`, `actions.qsa`만 사용해도 된다.
`state.db`는 Q121에서 활성화된 compact dirty-check DB다. `deps.db`는 Q123에서 활성화된
depfile-discovered header DB다.
파일 확장자는 internal이고 public API가 아니다.

Q120 MVP는 다음 파일을 실제로 쓴다.

- `manifest.json`: request identity, QStar version, generator, build dir, profile input,
  authoring input fingerprint summary.
- `inputs.json`: evaluated authoring inputs의 path/size/mtime/content hash.
- `graph.qsg`: validated Graph IR의 internal binary snapshot.
- `actions.qsa`: requested build root의 executable lowered action plan.

Q120은 Lua eval과 validation을 건너뛰는 whole-graph cache MVP였다. Q124부터 scheduler는
cache hit 시 `actions.qsa`에서 compile/archive/link action의 argv, output, description,
static input list를 직접 복원한다. 파일 content/env/action key는 실행 직전에 다시 계산하므로
stale source/header 변경은 기존 dirty-check와 같은 방식으로 rebuild된다.
Q121 이후에는 build start에서 `build/qstar/state/state.db`를 먼저 읽고, stale/missing이면
기존 build directory 호환을 위해 `state/actions.json`으로 fallback한다. Q132 이후
`state.db`가 Stella dirty-check의 canonical fast path이고, `state/actions.json`은
`QSTAR_DEBUG_STATE_DUMPS=1`을 설정했을 때만 쓰는 debug/export dump다.
Q123 이후에는 `build/qstar/state/deps.db`도 함께 읽는다. 이 DB는 compiler depfile 자체를
매번 다시 파싱하기 전에, depfile path/size/mtime/content hash가 그대로인지 확인하고
저장된 discovered header list를 재사용한다.

## Atomicity Policy

Cache write는 항상 atomic replacement로 처리한다.

1. `build/qstar/stella/tmp/<name>.<pid>.tmp`에 쓴다.
2. flush/close가 성공한 뒤 target file로 rename한다.
3. manifest는 가장 마지막에 쓴다.
4. manifest write가 실패하면 다음 invocation은 cache miss로 처리한다.

Partial cache는 error가 아니라 miss다. Stella는 cache가 부서져도 source-of-truth인
`qstar.lua`부터 다시 lowering할 수 있어야 한다.

## Cache Schema

`manifest.json`은 최소 다음 field를 가진다.

```json
{
  "schema": "qstar-stella-plan-cache-v1",
  "qstar_version": "0.5.1-beta.1",
  "plan_abi": 1,
  "package_root": "/absolute/package/root",
  "build_dir": "build/qstar",
  "generated_dir": "build/qstar/generated",
  "generator": "stella",
  "profile": "default",
  "target": "host",
  "toolchain": "",
  "stdlib": "",
  "compile_commands": "build",
  "input_fingerprint": "hex",
  "graph_fingerprint": "hex",
  "action_count": 0,
  "target_count": 0,
  "generated_action_count": 0
}
```

Absolute path는 package root identity 확인에만 사용한다. Action input/output path는
계속 package-relative 또는 build-dir-relative canonical path로 저장한다.

## Input Fingerprint

Fingerprint는 "Lua eval 결과가 바뀔 수 있는 모든 입력"을 포함한다.

### Required Inputs

| Category | 포함 항목 |
| --- | --- |
| Runtime | `QSTAR_VERSION`, plan cache ABI, host OS/arch |
| Entrypoint | `--file`로 선택된 `qstar.lua` absolute path와 file fingerprint |
| Imports | `qstar.subdir`, `qstar.import_file`, `qstar.import_module`로 읽은 모든 `.qst`/`.qsm` |
| CLI overrides | `-B`, `-G`, selected subcommand, build label, package aliases |
| Profile input | `--profile`, `--target`, `--toolchain`, `--stdlib` |
| Project options | effective `build_dir`, `generated_dir`, `compile_commands`, project root/name/version |
| Environment | QStar가 graph evaluation에 명시적으로 읽는 env only |
| Tool policy | profile tool names, `path_tools`, response style, target triple |
| Module rules | module entry resolution `<folder>/<name>.qsm` and imported module closure |

### File Fingerprint

각 authoring input file은 다음 tuple로 기록한다.

```txt
path=<package-relative-or-entry-absolute>
size=<bytes>
mtime_ns=<nanoseconds-if-available>
hash=<content-hash>
```

초기 구현은 correctness를 위해 content hash를 항상 계산한다. 나중에 성능상 필요하면
size/mtime fast path를 추가하되, mtime granularity가 낮거나 timestamp가 뒤로 이동한 경우에는
hash로 fall back한다.

### Environment Fingerprint

환경 변수는 무작정 전부 넣지 않는다. 전체 environment를 fingerprint하면 불필요한 miss가
너무 많아진다.

포함:

- QStar가 명시적으로 읽는 `QSTAR_*` 변수
- docs lookup이나 diagnostics에만 쓰이는 변수는 plan fingerprint에서 제외
- compiler execution에 영향을 주는 env는 action key/material에 넣고 plan fingerprint에는
  넣지 않는다

예:

```txt
QSTAR_DOC_DIR      -> exclude from plan fingerprint
QSTAR_TEST_QSTAR   -> exclude from plan fingerprint
QSTAR_PROFILE      -> include only if it affects selected profile input
PATH               -> exclude from plan fingerprint; resolved tool identity/action key handles tools
```

## Invalidation Conditions

Plan cache는 다음 상황에서 miss로 처리한다.

| Condition | 처리 |
| --- | --- |
| `manifest.json` missing or unreadable | miss |
| schema mismatch | miss |
| `qstar_version` mismatch | miss |
| plan ABI mismatch | miss |
| package root mismatch | miss |
| `-B` effective build dir mismatch | miss |
| generator is not `stella` | miss |
| requested label not covered by cached closure | miss |
| selected profile/target/toolchain/stdlib mismatch | miss |
| project `generated_dir` or `compile_commands` mismatch | miss |
| any authoring file path/size/mtime/hash mismatch | miss |
| imported file set differs | miss |
| package alias map differs | miss |
| graph validation policy version mismatch | miss |
| cache read parse error | miss and rewrite |

Cache miss는 normal condition이다. 일반 build output에는 noisy하게 출력하지 않는다.
`--verbose`, `--schedule-trace`, 또는 future `qstar doctor` cache section에서만 이유를
보여준다.

## Partial Reuse

Q120 MVP는 whole-plan hit/miss만 지원한다.

허용하지 않는 것:

- file 하나만 바뀌었을 때 partial Graph IR patch
- target 하나만 relower
- `.qsm` helper function 단위 cache

이유는 stale graph 위험이 크기 때문이다. Partial reuse는 plan cache v2 이후에 검토한다.

## Cached Plan Content

`actions.qsa`는 scheduler가 compile/archive/link action을 다시 materialize하지 않고 실행할 수
있는 최소 정보를 담는다. Generated/custom action은 이번 단계에서 기존 경로를 유지한다.

필수 field:

- action id
- kind: compile, archive, link, link-shared
- owner label
- source path if any
- output paths
- depfile path if any
- argv vector or argv digest plus argv table
- user-facing description
- static input list
- depfile-discovered input list snapshot

제외 field:

- full diagnostic explanation strings
- verbose schedule trace strings
- replay file paths
- stdout/stderr log paths
- cache miss reason strings

제외 field는 실행 직전 또는 failure path에서 lazy materialize한다.

## Graph Summary Content

`graph.qsg`는 action execution에 직접 필요하지 않은 graph metadata를 담는다.

포함:

- target label/name/kind
- origin file/line
- config count and selected config labels
- generated action labels
- stage labels
- project metadata
- compile database policy

용도:

- `qstar explain` cache status
- `qstar list-targets` fast path 후보
- future LSP/project index 후보

Q120에서는 build execution path만 먼저 사용하고, explain/list-targets fast path는 deferred로
둔다.

## Action State Boundary

Plan cache와 action state는 다르다.

| 파일 | 의미 |
| --- | --- |
| `actions.qsa` | 어떤 action을 실행할 수 있는지 |
| `state/actions.json` | 지난 build에서 action이 어떤 key/status였는지, opt-in debug/export dump |
| `state.db` | canonical compact fast dirty-check lookup |
| `deps.db` | depfile-discovered header list lookup |

Q121 이후 Stella는 `state.db`를 먼저 읽는다. compact DB가 없거나 schema/ABI가 맞지 않으면
조용히 기존 `state/actions.json`으로 돌아간다. Q132 이후 JSON state는 fast path에서
기본 생성하지 않는다. 사람이 읽는 debugging/export dump가 필요하면
`QSTAR_DEBUG_STATE_DUMPS=1 qstar build ...`처럼 opt-in으로 생성한다.
Q123 이후 Stella는 compile action의 depfile을 직접 파싱하기 전에 `deps.db`를 본다. depfile
fingerprint가 맞으면 cached header list를 쓰되, 각 header는 package-relative path와 존재
여부를 다시 검증한다. depfile fingerprint가 바뀌면 기존 depfile parser를 사용하고 성공한
compile 뒤 새 entry를 기록한다.

## Diagnostics Policy

Cache hit에서도 diagnostic 품질을 포기하지 않는다.

- build 실행 중 command failure는 기존 action log/replay diagnostic을 유지한다.
- stale cache가 의심되면 cache를 폐기하고 source-of-truth Lua eval로 돌아간다.
- cache parse error는 일반 build에서 warning으로 소란스럽게 만들지 않는다.
- `--schedule-trace`에서는 `plan_cache status=hit|miss reason=...`를 출력한다.
- `--schedule-trace`에서는 `deps_db status=hit|miss`도 출력한다.
- `qstar doctor`에는 future section으로 cache schema/version/input count를 보여줄 수 있다.

## Security And Trust

Plan cache는 build output이다. Source-of-truth가 아니다.

- cache file을 신뢰해서 arbitrary command를 새로 만들어내면 안 된다.
- cache hit은 fingerprint가 source tree와 일치할 때만 허용한다.
- package root 밖 path는 cache load 시에도 거부한다.
- action argv는 cached plan에서 읽더라도 original QStar validation policy를 통과한 결과로
  취급해야 하며, schema/version mismatch 시 즉시 miss 처리한다.

## Implementation Order

### Q120 MVP

1. authoring input tracking을 manifest로 serialize한다.
2. `actions.qsa` writer를 추가한다.
3. build start에서 manifest/input fingerprint를 확인한다.
4. hit이면 cached action plan을 load한다.
5. miss이면 기존 Lua eval path를 사용하고 plan cache를 rewrite한다.
6. `--schedule-trace`에 hit/miss reason을 출력한다.

Status: implemented. Cache hit이면 `plan_cache status=hit reason=hit`가
`--schedule-trace`에 출력된다. Cache miss는 일반 build output에서는 조용히 처리되고,
`--schedule-trace`에서만 `manifest-missing`, `authoring-input-changed`,
`request-mismatch` 같은 reason이 보인다.

### Q121 Fast State

Status: implemented.

### Q124 Direct Lowered Action Execution

Status: implemented.

`actions.qsa`는 ABI 2부터 executable lowered action plan이다. Cache hit path에서는
`--schedule-trace`에 다음과 같은 record가 나온다.

```txt
lowered_action id=//:app:compile:0 status=hit kind=compile
lowered_action id=//:app:link:0 status=hit kind=link
```

이 hit는 action argv/output/description 복원을 뜻한다. 실제 skip/run 여부는 이후
`state.db`, `deps.db`, file hash, output 존재 여부를 다시 확인한 뒤 결정된다. 즉 cache file만
있다는 이유로 action을 stale skip하지 않는다.

1. `build/qstar/state/state.db` compact lookup을 추가했다.
2. `state/actions.json`은 debug/export fallback으로 유지했다. Q132 이후 이 JSON dump는
   opt-in 생성으로 전환된다.
3. no-op/incremental에서 action state JSON parse를 피한다.
4. `--schedule-trace`에서 `dirty_state_db status=hit|miss`를 표시한다.
5. `why-rebuild`와 `--explain-cache`는 JSON/detail path를 유지한다.

### Q122 Process Runner

1. single action path와 scheduler path의 process wait code를 통합한다.
2. poll/select 기반 event wait를 추가한다.
3. warning/error stream coloring과 action log/replay를 유지한다.

Status: deferred. Q122 실험은 현 구조에서 유의미한 clean build 개선을 만들지 못해
revert했다. 이후 Q129는 `posix_spawn` start path를, Q130은 poll 기반 output drain path를
각각 별도 라운드로 적용했다. Clean build의 남은 격차는 더 큰 scheduler/process completion
boundary 정리가 필요하다.

### Q123 Compact Deps DB

Status: implemented.

1. `build/qstar/state/deps.db` compact lookup을 추가했다.
2. compile 성공 후 depfile에서 discovered header list를 읽어 `deps.db`에 저장한다.
3. 다음 build부터 depfile path/size/mtime/content hash가 그대로이면 depfile을 다시
   tokenize하지 않고 cached header list를 사용한다.
4. cached header도 package-relative path와 존재 여부를 다시 검사하므로 missing header
   diagnostic은 유지된다.
5. `--schedule-trace`에서 `deps_db status=hit|miss`를 표시한다.

### Q125 Clean Build Hot Path Batch Seal

Status: implemented.

1. Successful action metadata write path에 buffered file write를 적용했다.
2. `state.db`를 `state/actions.json`보다 먼저 쓰며, `actions.json`은 debug/export용
   fallback으로 유지한다. Q132 이후 `actions.json` write는 opt-in으로 내려간다.
3. Non-verbose progress renderer는 같은 percent tick을 반복 출력하지 않는다.
4. Plan cache store용 lowered action preparation은 compile database, depfile-discovered
   input scan, action key material hash 계산을 생략한다. 실제 build path에서는 기존처럼
   dirty-check key를 다시 계산한다.
5. Source input은 content digest로 추적하지만, `build_dir` 내부 generated object/archive
   input은 content를 다시 읽지 않고 path, size, mtime metadata만 key material에 섞는다.

Observed Q125 timing on the medium corpus:

```txt
medium_project_gate target_count=47 min_targets=40
medium_project_gate backend=stella phase=clean elapsed_ms=746
medium_project_gate backend=stella phase=noop elapsed_ms=69
medium_project_gate backend=stella phase=incremental elapsed_ms=91
medium_project_gate backend=ninja phase=clean elapsed_ms=254
medium_project_gate backend=ninja phase=noop elapsed_ms=73
medium_project_gate backend=ninja phase=incremental elapsed_ms=105
medium_project_gate status=ok perf_issue_count=0 report_only=1
```

Clean build는 Q124 대표값보다 개선됐지만 500-650ms 목표에는 아직 미달이다. 남은 개선은
process wait/drain 구조와 successful action log materialization을 더 크게 줄이는 라운드로
넘긴다.

### Q129 POSIX Spawn Runner MVP

Status: implemented.

1. compile/archive/link/custom generated action 실행 경로가 `spawn_action_process()` helper를
   공유한다.
2. macOS와 Linux/glibc에서는 package-root cwd file action과 stdout/stderr pipe redirection을
   유지한 채 `posix_spawn` fast path를 사용한다.
3. Spawn setup이 실패하거나 unsupported platform이면 기존 fork/exec runner로 fallback한다.
4. Timeout, cancel, warning/error stream coloring, action-log, replay는 기존 경로를 유지한다.
5. `--schedule-trace`에는 `runner=posix_spawn` 또는 `runner=fork`가 표시된다.

Observed Q129 timing on the medium corpus, local macOS arm64:

```txt
run 1: stella clean 1044ms, noop 88ms, incremental 116ms; ninja clean 389ms, noop 97ms, incremental 121ms
run 2: stella clean 833ms, noop 81ms, incremental 111ms; ninja clean 306ms, noop 88ms, incremental 121ms
run 3: stella clean 891ms, noop 81ms, incremental 105ms; ninja clean 446ms, noop 92ms, incremental 140ms
```

Q129은 process spawn boundary를 정리하는 구조 패치다. No-op과 incremental은 계속 Ninja급
체감권을 유지하지만, clean build는 machine state와 compiler process 비용에 따라 편차가
크고 아직 안정적으로 500-650ms 목표에 들어오지는 않는다. 다음 성능 라운드는 pipe wait/drain
event loop와 successful action log materialization 감소가 핵심이다.

### Q130 Event-Driven Output Drain

Status: implemented.

1. compile/custom action wait loop에서 fixed sleep pause를 제거했다.
2. Single action path, per-target parallel path, global ready-queue scheduler는 child
   stdout/stderr pipe fd를 `poll()`로 기다린 뒤 drain한다.
3. Warning/error stream coloring과 raw stdout/stderr action log는 그대로 유지한다.
4. Timeout/cancel propagation은 기존 UX를 유지한다.
5. macOS와 Linux는 같은 POSIX `poll()` path를 쓴다. Pipe capture가 없는 test artifact
   runner는 bounded `poll(NULL, 0, timeout)` fallback을 쓴다.

Observed Q130 timing on the medium corpus, local macOS arm64:

```txt
medium_project_gate target_count=47 min_targets=40
medium_project_gate backend=stella phase=clean elapsed_ms=821
medium_project_gate backend=stella phase=noop elapsed_ms=72
medium_project_gate backend=stella phase=incremental elapsed_ms=94
medium_project_gate backend=ninja phase=clean elapsed_ms=264
medium_project_gate backend=ninja phase=noop elapsed_ms=77
medium_project_gate backend=ninja phase=incremental elapsed_ms=104
medium_project_gate warning=stella clean 821ms exceeds ninja 264ms beyond ratio_x100=200 slack_ms=250
medium_project_gate status=ok perf_issue_count=1 report_only=1
```

Q130은 fixed sleep wait/drain 구조를 제거했지만, clean build wall time은 아직 개선으로
이어지지 않았다. No-op과 incremental은 Ninja급 latency를 유지한다. Q131은 successful action
log materialization을 lazy path로 옮겨 clean build metadata write 수를 줄인다. 남은 clean
gap은 process completion bookkeeping, compiler process count, remaining metadata write 쪽에
더 크게 남아 있다.

### Q131 Lazy Success Action Logs

Status: implemented.

1. Stella executor는 성공/skip action의 per-action `.log` 파일을 clean build hot path에서
   즉시 쓰지 않는다.
2. 실패 action, timeout, marker-missing, `last-failure` replay는 재현성을 위해 즉시 물리
   로그를 남긴다.
3. `qstar action-log <action-id>`와 `qstar replay <action-id>`는 물리 `.log`가 없으면
   compact state와 현재 graph에서 argv/description을 lazy 재구성한다.
4. `state.db`는 현재 build closure 밖의 이전 성공 action state를 보존해,
   다른 target을 빌드한 뒤에도 기존 action-log UX가 유지된다.
5. 물리 `build/qstar/logs/*.log` 파일 존재는 성공 action의 public contract가 아니다.

Observed Q131 timing on the medium corpus, local macOS arm64:

```txt
medium_project_gate target_count=47 min_targets=40
medium_project_gate backend=stella phase=clean elapsed_ms=1157
medium_project_gate backend=stella phase=noop elapsed_ms=83
medium_project_gate backend=stella phase=incremental elapsed_ms=95
medium_project_gate backend=ninja phase=clean elapsed_ms=269
medium_project_gate backend=ninja phase=noop elapsed_ms=79
medium_project_gate backend=ninja phase=incremental elapsed_ms=120
medium_project_gate warning=stella clean 1157ms exceeds ninja 269ms beyond ratio_x100=200 slack_ms=250
medium_project_gate status=ok perf_issue_count=1 report_only=1
```

Q131은 metadata write 수를 줄이는 구조 패치다. 이 corpus의 clean wall time에서는 아직 큰
개선으로 나타나지 않았고, no-op/incremental은 계속 Ninja급 latency를 유지한다. 다음 clean
build 성능 개선은 compiler process count 감소, action preparation batching, remaining
state/action metadata write 축소가 더 큰 효과를 낼 가능성이 높다.

### Q132 Debug State Opt-In And Perf Gate Refresh

Status: implemented.

1. `build/qstar/state/state.db`를 Stella dirty-check의 canonical fast path로 명확화했다.
2. `build/qstar/state/actions.json`은 fast path에서 기본 생성하지 않는다.
3. 사람이 읽는 action state dump가 필요하면 `QSTAR_DEBUG_STATE_DUMPS=1 qstar build ...`
   형식으로 opt-in한다.
4. 기존 build directory 호환을 위해 `state.db`가 없거나 stale이면 `actions.json` fallback
   read는 유지한다.
5. `state/graph.json`과 `state/last-summary.json`은 각각 graph snapshot과 build summary
   UX에 필요하므로 이번 라운드에서는 유지한다.

Observed Q132 timing on the medium corpus, local macOS arm64:

```txt
medium_project_gate target_count=47 min_targets=40
medium_project_gate backend=stella phase=clean elapsed_ms=991
medium_project_gate backend=stella phase=noop elapsed_ms=79
medium_project_gate backend=stella phase=incremental elapsed_ms=100
medium_project_gate backend=ninja phase=clean elapsed_ms=276
medium_project_gate backend=ninja phase=noop elapsed_ms=89
medium_project_gate backend=ninja phase=incremental elapsed_ms=107
medium_project_gate warning=stella clean 991ms exceeds ninja 276ms beyond ratio_x100=200 slack_ms=250
medium_project_gate status=ok perf_issue_count=1 report_only=1
```

Q132는 JSON debug dump write를 clean build hot path에서 제거했지만, clean wall time은
여전히 500-650ms 목표 범위에 닿지 못했다. No-op과 incremental은 Ninja급 latency를
유지한다. 남은 clean gap은 process completion bookkeeping, compiler process count,
remaining metadata write 쪽으로 본다.

### Q133-Q136 Scheduler Semantics Fixes

Status: implemented.

Q133-Q136은 clean build에서 Stella가 Ninja보다 크게 벌어지던 원인을 scheduler semantics와
link/archive path 쪽에서 정리했다.

1. Q133은 macOS default jobs detection을 고쳐 기본 `--jobs`가 host CPU count로 잡히게 했다.
   Multi-core host에서 `jobs=1` serial-ready-queue로 떨어지는 문제는 hard failure로 다룬다.
2. Q134는 `qstar.staticlib` final archive action이 dependency `.a`를 자기 archive argv에 다시
   넣지 않도록 고쳤다. Dependency static library는 order/build dependency일 뿐 archive member가
   아니다.
3. Q135는 dependent target compile action이 dependency target final archive를 기다리던
   과보수 edge를 완화했다. Usage requirement는 graph merge 결과로 반영하고, archive/link
   artifact dependency는 final action 단계에만 둔다.
4. Q136은 archive/link final action을 Stella async prepared action queue에 태웠다.
   `QSTAR_SCHED_FINAL`이 동기 `run_action()` 경로를 타며 후반부에서 serial처럼 멈칫하는 구조를
   제거했다.

### Q137 Stella/Ninja Performance Seal

Status: implemented.

Q137은 위 수정들이 실제 medium corpus에서 어떤 효과를 내는지 line protocol로 봉인한다.
Gate는 timing만 보지 않고, scheduler 구조가 기대대로 작동하는지도 확인한다.

Hard checks:

1. `default_jobs`가 multi-core host에서 1로 떨어지지 않는다.
2. 초기 ready queue width가 1보다 넓다.
3. archive/link final action이 `kind=archive|link` schedule action으로 잡히고,
   `kind=final state=ready` 동기 trace가 남지 않는다.
4. staticlib archive argv가 dependency `.a`를 포함하지 않는다.

Observed Q137 timing on the medium corpus, local macOS arm64:

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

이 측정에서는 Stella clean이 Ninja 대비 2배 이내 목표를 넘어서 1.5배 이내에도 들어왔다.
다만 작은 medium corpus는 host filesystem cache, compiler warm state, terminal load에 민감하다.
따라서 timing ratio는 계속 report-only로 둔다. Q137의 핵심은 "성능이 좋아졌다"는 인상보다,
default jobs, ready queue width, async final action, staticlib argv parity를 gate에서 함께
봉인했다는 점이다.

남은 성능 작업은 다음 순서가 적절하다.

1. 더 큰 synthetic corpus를 추가해 compiler process count가 늘어날 때도 1.5-2배 범위를
   유지하는지 본다.
2. generated/run action을 prepared-action model에 통합할지 별도 라운드에서 결정한다.
3. graph snapshot과 build summary write를 release/debug 필요도에 따라 더 줄인다.
4. Linux CI에서 같은 line protocol을 수집해 macOS-local 수치만으로 판단하지 않게 한다.

### Q138 Large Synthetic Corpus Gate

Status: implemented.

Q138은 medium corpus보다 큰 synthetic project shape를 별도 report gate로 추가한다. Medium
gate는 release smoke와 beta readiness 대표값이고, large gate는 scaling 관찰용이다.

Large gate는 기본적으로 200 target과 500 target mode를 만든다. 각 mode는 staticlib fanout,
mode별 executable link shard, `qstar.group "all"`, fake external compiler 기반 object artifact
bridge를 포함한다. Link shard는 500 target mode에서 argv-limit test가 아니라 scheduler
scaling test가 되도록 staticlib dependency를 나누어 가진다. Object artifact bridge는 새
language provider를 추가하지 않고 `qstar.custom_target`이
`qstar.output(..., {format = "object"})`를 만들고, staticlib가 그 object를 source로 소비한 뒤
executable shard가 해당 staticlib를 link dependency로 받는 구조다.

Line protocol은 `large_project_gate` prefix를 쓴다. Timing은 report-only이고, graph/build
failure, compile database 누락, generated object 누락, Ninja root `.ninja_log`/`.ninja_deps`
오염은 hard fail이다.

Observed Q138 timing on the large synthetic corpus, local macOS arm64:

```txt
large_project_gate mode=200 target_count=200 generated_actions=4 host_jobs=10
large_project_gate mode=200 backend=stella phase=clean elapsed_ms=1115
large_project_gate mode=200 backend=stella phase=noop elapsed_ms=77
large_project_gate mode=200 backend=stella phase=incremental elapsed_ms=115
large_project_gate mode=200 backend=stella-jobs jobs=10 phase=clean elapsed_ms=1123
large_project_gate mode=200 backend=ninja phase=clean elapsed_ms=2202
large_project_gate mode=200 backend=ninja phase=incremental elapsed_ms=146
large_project_gate mode=500 target_count=500 generated_actions=4 host_jobs=10
large_project_gate mode=500 backend=stella phase=clean elapsed_ms=2284
large_project_gate mode=500 backend=stella phase=noop elapsed_ms=98
large_project_gate mode=500 backend=stella phase=incremental elapsed_ms=139
large_project_gate mode=500 backend=stella-jobs jobs=10 phase=clean elapsed_ms=5171
large_project_gate mode=500 backend=ninja phase=clean elapsed_ms=2545
large_project_gate mode=500 backend=ninja phase=incremental elapsed_ms=204
large_project_gate status=ok perf_issue_count=0 report_only=1 modes="200 500"
```

첫 large gate에서는 Stella가 이 host에서 Ninja와 같은 성능권에 머물렀다. 다음 성능 작업은
medium-only micro-optimization보다 multi-run summary와 Linux CI 수집이 더 중요하다.

## Acceptance Criteria

Q120 이후:

- authoring input이 unchanged이면 `--schedule-trace`에서 plan cache hit이 보여야 한다.
- `.qst`/`.qsm` 하나를 수정하면 plan cache miss가 나야 한다.
- QStar version을 바꾸면 plan cache miss가 나야 한다.
- selected profile이나 `-B`가 바뀌면 plan cache miss가 나야 한다.
- `make check`와 `make qstar-medium-project-readiness-tests`가 통과해야 한다.

Observed Q121 timing on the medium corpus:

```txt
medium_project_gate target_count=47 min_targets=40
medium_project_gate backend=stella phase=clean elapsed_ms=745
medium_project_gate backend=stella phase=noop elapsed_ms=70
medium_project_gate backend=stella phase=incremental elapsed_ms=111
medium_project_gate backend=ninja phase=clean elapsed_ms=304
medium_project_gate backend=ninja phase=noop elapsed_ms=80
medium_project_gate backend=ninja phase=incremental elapsed_ms=120
medium_project_gate status=ok perf_issue_count=0 report_only=1
```

Q121 이후 no-op과 incremental은 compact dirty state + plan cache hit 경로에서 Ninja와 같은
체감권을 유지한다. Q123의 `deps.db`는 header가 많은 C/C++/freestanding project에서
depfile tokenization 비용을 줄이는 incremental hot-path 개선이다. Clean build는 여전히
compiler/process orchestration과 logging overhead 때문에 Ninja보다 느리며, 다음 큰 lever는
process runner와 scheduler hot path다.

Observed Q123 representative timing on the medium corpus:

```txt
medium_project_gate backend=stella phase=clean elapsed_ms=808
medium_project_gate backend=stella phase=noop elapsed_ms=82
medium_project_gate backend=stella phase=incremental elapsed_ms=103
medium_project_gate backend=ninja phase=clean elapsed_ms=263
medium_project_gate backend=ninja phase=noop elapsed_ms=74
medium_project_gate backend=ninja phase=incremental elapsed_ms=105
medium_project_gate warning=stella clean 808ms exceeds ninja 263ms beyond ratio_x100=200 slack_ms=250
medium_project_gate status=ok perf_issue_count=1 report_only=1
```

## Open Questions

- Q120에서 `qstar list-targets`까지 cache fast path를 열 것인가?
  - 결정: build path만 먼저 적용한다.
- `actions.qsa`를 JSON으로 시작할 것인가 line protocol로 시작할 것인가?
  - 결정: magic line + root JSON line + binary payload로 둔다. 파일 정체와 root는 사람이 바로
    확인할 수 있고, action payload는 parse overhead를 줄인다.
- content hash를 항상 계산하면 no-op이 느려지지 않는가?
  - 결정: MVP는 correctness 우선으로 항상 hash한다. 이후 mtime/size fast path를 추가한다.
- cache miss reason을 일반 output에 보여줄 것인가?
  - 결정: 일반 output에서는 숨긴다. `--schedule-trace`와 future doctor에서만 보여준다.

## Q119 Verdict

Stella plan cache는 QStar를 Ninja처럼 낮은 수준 build executor로 바꾸는 작업이 아니라,
QStar의 고수준 authoring phase와 Stella의 execution phase를 분리하는 작업이다. Q120은
whole-plan cache MVP로 시작하고, partial cache나 LSP index reuse는 그 이후로 미룬다.
