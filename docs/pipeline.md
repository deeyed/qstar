# QStar Build Pipeline

QStar v0 pipeline은 explain-first다. 실제 executor는 다음 단계로 둔다.

```txt
v0:
  graph
  query
  check
  explain
  command-plan
  restricted local build

not v0:
  full executor
  Ninja generator
  package registry
  binary cache
```

Round 1 implemented the first executable slice as a standalone binary:

```txt
qstar/build/bin/qstar
  -> sandboxed Lua 5.4 evaluator
  -> minimal Graph IR
  -> deterministic explain dump
```

Round 2 extends that binary without changing the C frontend/backend pipeline:

```txt
qstar/build/bin/qstar
  -> sandboxed Lua 5.4 evaluator
  -> canonical Graph IR
  -> target closure validation
  -> dependency-first order
  -> deterministic non-executing command plan
```

Round 3 adds package/profile input plumbing around the same non-executing
pipeline:

```txt
CLI package alias map
CLI profile input
  -> Graph IR context
  -> external dependency explanation
  -> deterministic command plan
```

Round 8 keeps the binary standalone but allows `install-user` to install it
is installed only by `make -C qstar install`. `downstream build` does not call it yet.

Round 9 adds authoring checks:

```txt
qstar/build/bin/qstar
  -> sandboxed Lua 5.4 evaluator
  -> canonical Graph IR
  -> package-root file existence checks
  -> deterministic check summary
```

Round 10/11 add query and deterministic source expansion:

```txt
qstar/build/bin/qstar
  -> origin-aware diagnostics
  -> list-targets/query/doctor
  -> qstar.files glob/exclude expansion
  -> Lua helper host branching over qstar.host
```

Round 12/13 add the first executable edge while keeping QStar standalone:

```txt
qstar/build/bin/qstar
  -> qstar.profile in-DSL profile input
  -> builtin host/clang/external-tool toolchain resolver
  -> real argv command plan
  -> restricted local executor v1 for qstar build
```

Round 14/15 keep QStar independent from the root external language Makefile and add repeated
build UX:

```txt
qstar/build/bin/qstar
  -> build/qstar/state/state.db canonical compact dirty-check state
  -> build/qstar/state/actions.json opt-in debug/export action dump
  -> cache-hit skip for unchanged local actions
  -> compile_commands.json
  -> why-rebuild / clean / log / last-failure
  -> text or JSON diagnostics
```

Round 16/17 add C/external source command integration and generated-output chaining
without touching the compiler frontend/backend internals:

```txt
qstar/build/bin/qstar
  -> source kind: .c, .h, .foreign, generated .c, generated header
  -> host/clang/external-tool argv rendering for C source
  -> external-tool process invocation for .foreign object compile
  -> generated source/header producer ordering
  -> qstar.configure_file
  -> generated header cache-key participation
```

Round 18/19 broaden local developer loop behavior while QStar remains
standalone:

```txt
qstar/build/bin/qstar
  -> public/private dependency policy
  -> public/private/interface include propagation
  -> staticlib transitive link order
  -> profile-shaped system link flags
  -> initial sharedlib stable unsupported local executor policy
  -> qstar.test target and qstar test runner
  -> qstar install --prefix skeleton
```

Round 20 seals QStar as a v0 developer build system:

```txt
qstar/build/bin/qstar
  -> standalone build/check/install under qstar/Makefile
  -> manual sample corpus
  -> v0 compatibility contract
  -> qstar-v0-release-tests aggregate
```

Round 38 seals the hardened standalone v0.1 surface:

```txt
qstar/build/bin/qstar
  -> executor/profile/install/test/compile database integrated smoke
  -> full manual and project sample corpus release gate
  -> persistent graph snapshot and action replay UX
  -> qstar-v0.1-release-tests aggregate
  -> downstream build integration remains deferred
```

Round 55 adds boot/package staging without making UEFI or RPi a hardcoded target
kind:

```txt
qstar/build/bin/qstar
  -> qstar.stage copy-only package tree
  -> qstar.stage_file plain file or target artifact mapping
  -> ESP layout such as EFI/BOOT/BOOTX64.EFI
  -> RPi layout such as config.txt, kernel8.img, payload
  -> build/qstar/stage/<label>/manifest.json and dry-run diff
```

Round 56 strengthens `qstar.run_target` as the external smoke wrapper surface:

```txt
qstar.run_target
  -> QEMU or emulator wrapper as qstar.cli argv-vector
  -> stdout/stderr plus optional marker_log scan
  -> marker-missing / timeout / exit-code result split
  -> last-failure and replay command for failed smoke actions
```

Round 21/22 make that v0 surface easier to author and less C/external-specific:

```txt
qstar/build/bin/qstar
  -> qstar init c-app|c-lib|generated|generated-extra
  -> sample/init drift checks
  -> source kind registry
  -> target rule registry
  -> provider-like output groups
```

Round 23/24 add practical dependency tracking and C++ toolchain surface:

```txt
qstar/build/bin/qstar
  -> C/C++ compile depfile readback
  -> depfile-discovered package-local headers in action keys
  -> missing discovered header diagnostics
  -> .cc/.cpp/.cxx/.hpp source kind surface
  -> c++/clang++ command rendering
  -> C/C++ mixed target build
  -> C++ module stable unsupported gate
```

## Implementation Staging

QStar는 C compiler, frontend driver, backend, package manager와 접점이 많다. 따라서 C lane이나 libc/target compatibility 작업이 진행 중인 동안에는 QStar를 compiler frontend/backend와 분리해 독립 바이너리로 유지한다. Round 14/15 이후에는 작은 C-only local build까지 실행할 수 있지만, `downstream build` 통합은 여전히 보류한다.

초기 구현 순서는 다음을 권장한다.

1. Graph IR, label canonicalization, source/header path selection, command-plan text dump를 고정한다.
2. Build Plan IR와 action key material을 고정한다.
3. `qstar explain`/`dry-run` 형태의 non-executing standalone surface를 안정화한다.
4. 제한 local executor, incremental state, diagnostic/log UX를 QStar 내부에서만 연다.
5. C/external source process invocation을 QStar 독립 binary 안에서 먼저 검증한다.
6. QStar v0 sample corpus와 release gate를 봉인한다.
7. `qstar init`과 language-agnostic rule registry를 추가한다.
8. C lane이 안정화된 뒤 `downstream build` 연결을 별도 라운드에서 검토한다.

header-language grammar와 C declaration import/export 모델은 external compiler/header-language checker 쪽
설계다. QStar는 `.h`을 특별히 읽지 않고 build graph의 header file path로만
취급한다.

이 staging은 QStar를 늦추기 위한 것이 아니라, C11/C17/C23 compatibility 작업과 build-system 변경이 같은 파일을 동시에 건드리는 충돌을 줄이기 위한 경계다.

## Explain-First Pipeline

초기 pipeline은 다음 순서다.

1. Root `qstar.lua`와 subdir `<dirname>.qst`를 평가한다.
2. `qstar.profile` declaration과 CLI `--profile` selection으로 active profile을 확정한다.
3. Package manager가 resolved package root map을 제공한 경우 label resolver input으로 소비한다.
4. Internal canonical Graph IR를 생성한다.
5. Graph validation을 수행한다.
6. `qstar.files`와 일반 Lua helper 결과로 명시 source selection을 확정한다.
7. Target closure, module set, header set, source selection을 계산한다.
8. `qstar check`/`doctor`/`build`/`why-rebuild`이면 package-root 기준 authoring file existence를 확인한다.
9. Command plan과 argv plan을 생성한다.
10. Generated source/header producer를 target source/header consumer보다 먼저 배치한다.
11. `qstar query`, `qstar explain`, `qstar dry-run` 등으로 출력하거나 `qstar build`에서 제한 실행한다.
12. `qstar build`는 action state를 갱신하고 `compile_commands.json`을 쓴다.

`explain`과 `dry-run`은 실제 compiler/linker process를 실행하지 않는다.
`build`만 Round 13의 제한 executor policy 안에서 process를 실행한다.

## Inputs

Pipeline input:

- `qstar.lua`
- `<dirname>.qst`
- `qstar.profile` declarations inside QStar files
- resolved package root map
- selected target/profile/toolchain
- command-line build options

QStar file은 dependency version을 resolve하지 않는다. Package manager가 `@pkg` alias를 이미 resolve해야 한다.

## QStar Evaluation

QStar file evaluation은 deterministic해야 한다.

- arbitrary network 금지
- arbitrary file I/O 금지
- arbitrary process spawn 금지
- current time/random 금지
- global mutable state 금지

평가 결과는 Graph IR node list와 edge list다.

## Command Plan

Command plan은 build 실행 전 단계다.

Command plan에는 다음이 들어갈 수 있다.

- compile command
- generated source action command
- archive/link command
- include path
- module root
- selected source list
- selected public header list
- toolchain/stdlib policy

Command plan은 설명 가능해야 한다.

```txt
qstar --file qstar.lua explain //:app
```

Round 2 command-plan dump는 다음 invariant를 가진다.

- `closure-order`는 dependency-first order다.
- unknown dependency label은 stable diagnostic이다.
- unresolved external package label은 package resolver가 아직 없으므로 stable diagnostic이다.
- dependency cycle은 stable diagnostic이다.
- `action compile`, `action archive`, `action link`, `action link-shared`,
  `action compile-objects`는 설명용 plan line일 뿐 process 실행이 아니다.

Round 3 package/profile skeleton invariant:

- `--package-alias @name=/path` only records a resolved package root hint.
- External labels with a known alias are reported as `external_dep`.
- External labels without an alias remain stable diagnostics.
- QStar does not load external `qstar.lua` files in Round 3.
- `--profile`, `--target`, `--toolchain`, `--stdlib` are explain inputs, not
  full toolchain/profile resolvers.

Round 4/5 header graph invariant:

- public headers are package-relative file paths under `include/`.
- private headers are package-relative file paths.
- QStar does not parse or classify header-language, C, C++, or external language header contents.
- `.h` is opaque to QStar; header-language semantics belong to the compiler/header-language checker.

Round 5 Build Plan IR invariant:

- every planned action has a deterministic `action_key` material line.
- action keys include action kind, owner target, input, output, profile, target,
  target-local toolchain/stdlib, dependency list, and package alias map.
- action keys are key material, not final cache hashes.
- command-plan output remains non-executing.

Round 6 source/toolchain skeleton invariant:

- explicit `sources` entries must be package-relative paths.
- accepted source suffixes are `.c`, `.foreign`, `.s`, and `.S`.
- QStar prints `source_discovery`, `source_file`, and `command_skeleton`
  records for explainability.
- `command_skeleton` records are not shell commands and must not execute tools.
- target-local toolchain/stdlib and CLI target/profile input are preserved as
  key material, but no toolchain resolver is run yet.

Round 7 generated-output/toolchain-resolver invariant:

- `qstar.custom_target` creates generated action skeletons.
- generated outputs must be package-relative paths under the effective
  `qstar.project.generated_dir`; the default is `generated`.
- generated source paths must have exactly one producer.
- `qstar.output(path)` is a path spelling helper, not an execution primitive.
- `resolved_toolchain` records expose target-local toolchain/stdlib plus CLI
  profile/target input as resolver skeleton data.
- generator, compiler, archiver, and linker processes are still not executed.

Round 16/17 generated-output invariant:

- `qstar.custom_target` output can be consumed by `sources`, `public_headers`, or
  `private_headers`.
- generated outputs must still be package-relative paths under the effective
  `qstar.project.generated_dir`.
- generated source/header consumers trigger the producing action before compile.
- two generated actions cannot claim the same output.
- outputs outside the package root or outside the effective generated root are stable
  diagnostics.
- generated header content is included in dependent compile action keys.
- `qstar.configure_file` is an internal generated action that does not spawn a
  process.

External object bridge invariant:

- `.c` source is compiled by the selected C tool role.
- External language source is not a QStar compile provider.
- External compiler invocation is expressed with `qstar.custom_target`.
- Generated object output is marked with `qstar.output(path, {format = "object"})`.
- QStar does not call compiler frontend/backend internal APIs.
- `.h` paths are header inputs, not compile sources; listing `.h` in `sources`
  is a stable diagnostic.
- unsupported suffixes remain stable diagnostics.

Round 18 link policy invariant:

- `deps` and `public_deps` are public build dependencies.
- `private_deps` are build/link dependencies whose include directories do not
  propagate to consumers.
- `public_include_dirs` apply to the owning target and propagate through public
  dependencies.
- `interface_include_dirs` propagate to consumers but do not apply to the owner.
- `private_include_dirs` apply only to the owning target.
- staticlib dependency artifacts are linked in dependent-before-dependency
  order.
- duplicate dependency/library/link framework declarations are stable diagnostics.
- `libs` and `lib_dirs` are rendered as target-shaped argv flags. macOS
  frameworks are represented only as `link.frameworks` and render to
  `-framework <name>` on Darwin-like targets.
- `sharedlib` builds and installs on Darwin/Linux-like profiles. Consumers get
  build-tree runtime rpath flags based on `@loader_path` or `$ORIGIN`. Windows
  runtime `.dll`, import `.lib`, and PDB/debug policy rejects with a stable
  diagnostic.

Round 19 developer-loop invariant:

- `qstar.test` declares a test executable target.
- `qstar test //:unit` builds the test and executes the artifact.
- `qstar test //...` runs every local test target in graph order.
- test stdout/stderr are stored in `build/qstar/logs`.
- a 5-second timeout, nonzero exit, and signal termination are reported as
  stable failures.
- `qstar install --prefix` copies built exe/staticlib artifacts and public
  headers only.
- install dry-run reports planned copies without requiring artifacts to exist.

Round 8 dry-run executor skeleton invariant:

- `qstar dry-run <label>` uses the same validated target closure as
  `qstar explain`.
- dry-run output is deterministic executor-shaped text, not shell commands.
- generated actions, compile actions, archive actions, and link actions are
  printed as `dry_run_step` records.
- every `dry_run_step` has `execute=no`.
- no generator, compiler, archiver, linker, package fetcher, cache process, or
  Ninja invocation is spawned.
- the QStar manual fixture under `qstar/tests/manual/hello` is the current
  hand-authored smoke surface for direct user experimentation.

Round 9 authoring-check invariant:

- `qstar check <label>` uses the same target closure validation as
  `qstar explain`.
- `qstar check` validates real source files, public/private headers, and
  generated action inputs under the package root.
- generated outputs are not required to exist when exactly one
  `qstar.custom_target` produces them.
- `qstar check` does not execute generators, compilers, archivers, linkers,
  package fetchers, or Ninja.
- `qstar check` is stricter than `explain` and `dry-run`; those commands remain
  useful while authoring a graph before every file exists.

Round 10 diagnostic/query invariant:

- target and generated-action declarations keep Lua source file and line.
- validation diagnostics attach field/label origin where the graph owner is
  known.
- `--diagnostics json` emits `qstar-diagnostic-v1` JSON diagnostics for future
  LSP/editor integration. `--diagnostic-format line` remains a compatibility
  alias.
- `qstar list-targets`, `qstar query`, and `qstar doctor` do not execute build
  actions.

Round 11 source-selection invariant:

- `qstar.files` accepts package-relative literal paths and simple single-directory
  glob patterns.
- `qstar.files { ..., exclude = {...} }` filters literals or wildcard patterns.
- glob expansion is deterministic and unmatched globs are stable diagnostics.
- duplicate sources inside one target are stable diagnostics.
- Host-specific source branching uses ordinary Lua `if` over `qstar.host.os` and
  `qstar.host.arch`; Graph IR stores only the selected concrete list.

Round 12 profile/toolchain resolver invariant:

- `qstar.profile` is the only built-in profile declaration surface.
- supported keys include target/toolchain/stdlib plus compiler, linker, sysroot,
  response-file, external-tool, freestanding, and artifact policy.
- CLI profile/target/toolchain/stdlib input overrides QStar DSL input.
- builtin toolchain profiles are `host`, `clang`, and `external-tool`.
- command rendering emits `command_argv` records with deterministic `build/qstar/out`
  paths; this is argv data, not shell text.

Round 13 local executor invariant:

- `qstar build <label>` executes only package-local generated tools, C compile,
  static archive, and executable link actions.
- generated action tools must be package-relative paths.
- stdout, stderr, and action logs are stored under `build/qstar/logs`.
- build artifacts are stored under `build/qstar/out`.
- `.foreign`, assembly, remote packages, cache, and Ninja execution remain out of
  scope for v1.

Round 14 incremental-state invariant:

- QStar is built and checked from `qstar/`; the root external language `Makefile` does not
  own QStar targets.
- `build/qstar/state/state.db` is the canonical compact internal dirty-check state
  loaded first by Stella.
- `build/qstar/state/actions.json` is opt-in debug/export metadata written when
  `QSTAR_DEBUG_STATE_DUMPS=1` is set.
- `build/qstar/state/graph.json` and success `build/qstar/state/last-summary.json`
  are opt-in debug/export metadata written when `QSTAR_DEBUG_STATE_DUMPS=1` or
  `--schedule-trace` is set.
- failure `build/qstar/state/last-summary.json` is still written immediately.
- `build/qstar/state/deps.db` is the compact internal depfile-discovered header
  state loaded before Stella reparses compiler depfiles.
- action key v1 includes argv, declared input path metadata/content hash, output
  path, selected profile/toolchain, and a small environment whitelist.
- unchanged actions are skipped only when the key matches and every declared
  output still exists.
- `qstar why-rebuild <label>` reports cache status without executing actions.
- `qstar clean` removes `build/qstar` and `compile_commands.json`; `qstar clean
  --target <label>` removes target output directories.
- `compile_commands.json` is regenerated by `qstar build`.

Round 15 diagnostics/log invariant:

- build output reports per-action `status=run`, `status=skip`, or `status=fail`.
- stdout/stderr logs and failure action logs stay under `build/qstar/logs`.
- successful Stella action logs are logical and may be reconstructed by
  `qstar action-log`/`qstar replay` instead of existing as physical `.log` files.
- `build/qstar/logs/last-failure.replay` includes the package-root cwd and argv.
- `qstar log <label>` lists matching logical action logs.
- `qstar last-failure` prints the replay file when present.
- `--diagnostics json` emits one JSON object per top-level error.
- `qstar doctor` reports package root, profile input, writable state dir, and
  builtin toolchain resolver sanity.

## Future: Ninja Generator

Ninja generator는 v0 범위가 아니다. Future 단계에서 Graph IR와 command plan을 Ninja build file로 내보낼 수 있다.

장점:

- 빠른 incremental build
- 검증된 executor
- 대형 프로젝트 대응

주의:

- Ninja는 QStar의 package/profile resolver가 아니다.
- Ninja file은 generated artifact다.

## Future: Internal Executor

Round 13의 local executor v1은 작은 C-only package를 검증하기 위한 제한 executor다.
Round 14/15부터 이 executor는 incremental state와 기본 diagnostic UX를 갖지만,
여전히 local developer executor다. Future 단계에서는 이를 full internal executor로
확장할 수 있다.

- command scheduling
- depfile tracking
- parallel execution
- cache key calculation
- sandbox policy
- action log

Internal executor는 freestanding/bootstrap 환경에 유리하지만 구현량이 크므로, v0는 explain-first로 둔다.

## Error Policy

QStar는 모호한 상황을 추측하지 않는다.

- unknown target은 error
- ambiguous package alias는 error
- incompatible target을 직접 build하면 error
- `qstar.lua`에서 secure profile이나 UB category override를 정의하면 error
- graph/build phase에서 network를 요청하면 error
