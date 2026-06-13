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
  actions.json        # debug/export action state
  state.db            # compact dirty-check action state, internal binary v1
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
`state/actions.json`으로 fallback한다.
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
| `state/actions.json` | 지난 build에서 action이 어떤 key/status였는지, debug/export용 |
| `state.db` | compact fast dirty-check lookup |
| `deps.db` | depfile-discovered header list lookup |

Q121 이후 Stella는 `state.db`를 먼저 읽는다. compact DB가 없거나 schema/ABI가 맞지 않으면
조용히 `state/actions.json`으로 돌아간다. JSON state는 사람이 읽는 debugging/export
surface로 유지한다.
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
2. `state/actions.json`은 계속 쓴다.
3. no-op/incremental에서 action state JSON parse를 피한다.
4. `--schedule-trace`에서 `dirty_state_db status=hit|miss`를 표시한다.
5. `why-rebuild`와 `--explain-cache`는 JSON/detail path를 유지한다.

### Q122 Process Runner

1. single action path와 scheduler path의 process wait code를 통합한다.
2. poll/select 기반 event wait를 추가한다.
3. warning/error stream coloring과 action log/replay를 유지한다.

Status: deferred. Q122 실험은 현 구조에서 유의미한 clean build 개선을 만들지 못해
revert했다. 실제 process runner 개편은 더 큰 scheduler/process boundary 정리가 필요하다.

### Q123 Compact Deps DB

Status: implemented.

1. `build/qstar/state/deps.db` compact lookup을 추가했다.
2. compile 성공 후 depfile에서 discovered header list를 읽어 `deps.db`에 저장한다.
3. 다음 build부터 depfile path/size/mtime/content hash가 그대로이면 depfile을 다시
   tokenize하지 않고 cached header list를 사용한다.
4. cached header도 package-relative path와 존재 여부를 다시 검사하므로 missing header
   diagnostic은 유지된다.
5. `--schedule-trace`에서 `deps_db status=hit|miss`를 표시한다.

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
