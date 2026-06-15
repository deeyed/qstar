# QStar Graph IR

> Current note: Round 47 이후 새 authoring syntax는
> `docs/qstar-v0.2-authoring-spec.md`와 `../wiki/`를 정본으로 본다. 이
> 문서의 text dump field 중 일부는 내부 normalized storage 이름을 그대로 보여준다.

QStar Graph IR는 QStar build file 평가 결과를 담는 내부 canonical graph representation이다.

설문 결정에 따라 v0 Graph IR는 public stable format이 아니다. QStar는 내부 canonical graph와 deterministic text dump만 고정한다.

```txt
Status:
  Internal canonical Graph IR
  deterministic text dump for explain/test/debug
  no stable JSON API in v0
  no stable binary format in v0
```

Round 1 implemented a minimal in-memory Graph IR in the standalone `qstar`
binary. Round 2 adds dependency closure validation, dependency-first order, and
non-executing command-plan dump. It stores target label, kind, module set,
source/header lists, include dirs, deps, toolchain, and stdlib policy. It does
not execute build actions.

Round 3 adds graph context for package aliases and profile input:

```txt
profile name=debug target=arm64-apple-macos toolchain=clang stdlib=external-tool
package_aliases [@core=/path/to/core]
```

These fields are explain/debug context. They are not a stable serialized graph
format and do not imply package fetch or external graph loading.

## Purpose

Graph IR는 다음을 위해 필요하다.

- `qstar explain`
- `qstar --dump-graph`
- command-plan 생성
- target dependency validation
- module/header/source selection 검증
- future Ninja generator
- future internal executor
- future cache key 계산

## Node Types

초기 Graph IR node는 다음을 가진다.

| Node | 의미 |
| --- | --- |
| package node | resolved package root와 package metadata reference |
| target node | `qstar.target`, `qstar.executable`, `qstar.staticlib` 등 산출물 |
| config node | `qstar.config` reusable target option bundle |
| module-set node | `qstar.modules { root, include, exclude }` |
| source-set node | explicit source, glob result, Lua helper result |
| header-set node | public/private headers와 include dirs |
| generated-action node | `qstar.custom_target` action |
| stage node | `qstar.stage` copy-only package/boot staging rule |
| toolchain reference | selected toolchain/profile reference |
| dependency edge | label-to-label dependency |
| host constants | `qstar.host.os`, `qstar.host.arch` values consumed during Lua evaluation |
| option node | `qstar.option` declaration |
| tool node | `qstar.tool` capability declaration |

## Labels

Graph IR에서 모든 target은 canonical label을 가진다.

```txt
//:app
//src/render:render
@core//src/mem:mem
```

Local label은 canonicalization 단계에서 full label로 바뀐다.

```txt
:local
  -> //current/path:local
```

## Host Branch Canonicalization

Host-specific authoring uses ordinary Lua `if` over `qstar.host.os` and
`qstar.host.arch`. Graph IR records only the selected concrete list; it does not
preserve a QStar-specific condition node.

```lua
local sources = {"src/platform/portable.c"}
if qstar.host.os == "macos" then
    sources = {"src/platform/darwin.c"}
elseif qstar.host.os == "linux" then
    sources = {"src/platform/linux.c"}
end
```

## Deterministic Text Dump

`qstar --dump-graph` and tests use a deterministic raw Graph IR text dump.

예:

```txt
target //:engine
  kind staticlib
  toolchain host
  stdlib system
  lang.cxx.modules root=src include=[asset, render]
  public_headers [include/engine/engine.h]
  lang.include_dirs [include]
  deps [//src/asset:asset, //src/render:render]
```

이 dump는 사람이 읽고 test fixture로 비교하기 위한 내부 진단 surface다. 외부 stable API가 아니다.

## Closure And Command Plan Dump

Round 2 makes `qstar explain <label>` a closure-aware command-plan dump.

```txt
qstar command plan v1
build-plan-ir v1
root //:app
profile name=default target=host toolchain=default stdlib=default
package_aliases []
generated_action //:version
  tool version-gen
  inputs [tools/version.txt]
  outputs [generated/version.c]
  args [--out, generated/version.c]
closure-order [//:core, //:app]
target //:core
  order 0
  kind staticlib
  deps []
  source_discovery explicit=1 modules=absent status=explicit-only
  source_file path=src/core.c language=c tool=c-compiler provider=c output_group=objects role=compile
  sources [src/core.c]
  public_headers [include/core.h]
  lang.include_dirs [include]
  system_include_dirs []
  toolchain host
  stdlib system
  resolved_toolchain owner=//:core toolchain=host profile=default target=host stdlib=system resolver=skeleton
  action compile source=src/core.c output=<object://:core:0>
  action_key id=//:core:compile:0 kind=compile owner=//:core input=src/core.c output=<object://:core:0> language=c profile=default target=host toolchain=host stdlib=system deps=[] packages=[]
  command_skeleton id=//:core:compile:0 phase=compile language=c tool=c-compiler toolchain=host target=host stdlib=system input=src/core.c output=<object://:core:0> execute=no
  action archive output=<artifact://:core>
  action_key id=//:core:archive:0 kind=archive owner=//:core input=<target-objects> output=<artifact://:core> language=artifact profile=default target=host toolchain=host stdlib=system deps=[] packages=[]
  command_skeleton id=//:core:archive:0 phase=archive language=artifact tool=archiver toolchain=host target=host stdlib=system input=<target-objects> output=<artifact://:core> execute=no
target //:app
  order 1
  kind exe
  deps [//:core]
  generated_edge source=generated/version.c generator=//:version output=generated/version.c
  generated_action id=//:version tool=version-gen inputs=[tools/version.txt] outputs=[generated/version.c] args=[--out, generated/version.c] execute=no
  action_key id=//:version:generate:0 kind=generate owner=//:version consumer=//:app input=[tools/version.txt] output=[generated/version.c] language=generated profile=default target=host toolchain=host stdlib=system deps=[] packages=[]
  command_skeleton id=//:version:generate:0 phase=generate language=generated tool=version-gen toolchain=host target=host stdlib=system input=[tools/version.txt] output=[generated/version.c] consumer=//:app execute=no
  source_discovery explicit=2 modules=absent status=explicit-only
  source_file path=generated/version.c language=c tool=c-compiler provider=c output_group=objects role=compile
  source_file path=src/main.c language=c tool=c-compiler provider=c output_group=objects role=compile
  sources [generated/version.c, src/main.c]
  public_headers []
  include_dirs []
  system_include_dirs []
  toolchain host
  stdlib system
  resolved_toolchain owner=//:app toolchain=host profile=default target=host stdlib=system resolver=skeleton
  action compile source=generated/version.c output=<object://:app:0>
  action_key id=//:app:compile:0 kind=compile owner=//:app input=generated/version.c output=<object://:app:0> language=c profile=default target=host toolchain=host stdlib=system deps=[//:core] packages=[]
  command_skeleton id=//:app:compile:0 phase=compile language=c tool=c-compiler toolchain=host target=host stdlib=system input=generated/version.c output=<object://:app:0> execute=no
  action compile source=src/main.c output=<object://:app:1>
  action_key id=//:app:compile:1 kind=compile owner=//:app input=src/main.c output=<object://:app:1> language=c profile=default target=host toolchain=host stdlib=system deps=[//:core] packages=[]
  command_skeleton id=//:app:compile:1 phase=compile language=c tool=c-compiler toolchain=host target=host stdlib=system input=src/main.c output=<object://:app:1> execute=no
  action link output=<artifact://:app>
  action_key id=//:app:link:0 kind=link owner=//:app input=<target-objects> output=<artifact://:app> language=artifact profile=default target=host toolchain=host stdlib=system deps=[//:core] packages=[]
  command_skeleton id=//:app:link:0 phase=link language=artifact tool=linker toolchain=host target=host stdlib=system input=<target-objects> output=<artifact://:app> execute=no
```

The action lines are plan records. They are not shell commands and do not spawn
the compiler, archiver, or linker.

Round 55부터 graph dump/list-targets JSON은 stage node도 보존한다. Stage node는
target dependency closure가 아니라 copy/package surface다. `qstar.stage_file`의 source는
package-relative file 또는 `qstar.target_file("//:label")` token으로 남고, executor가
`qstar stage`에서 staged manifest와 dry-run diff를 생성한다.

Round 5 action-key lines are deterministic key-material skeletons. They are not
cryptographic hashes yet. Future cache/executor layers may hash this material
after toolchain/profile/source discovery policy is complete.

Round 6 `source_discovery`, `source_file`, and `command_skeleton` lines are also
deterministic plan records. They are not shell commands and are not executed by
QStar.

Round 7 `generated_action` and `generated_edge` lines connect generated outputs
to consuming target sources. Round 68 adds `artifact_input_edge` for
`custom_target.inputs = { qstar.target_file("//:label") }`, so generated actions
can explicitly depend on compile/link target artifacts or another generated
action's primary output. These plan records make objcopy/image edges visible
before execution.

Round 8 adds `qstar dry-run <label>`, which reuses the validated graph closure
and projects it into executor-shaped records. Round 12 upgrades that projection
with real argv data and deterministic `build/qstar/out` output paths:

```txt
qstar dry-run v1
root //:app
closure-order [//src/foo:foo, //:app]
dry_run_target //src/foo:foo order=0 kind=staticlib
  resolved_toolchain owner=//src/foo:foo toolchain=host profile=default target=host stdlib=system resolver=builtin-v1 cc=cc external-tool=external-tool ar=ar linker=cc
  dry_run_step id=//src/foo:foo:compile:0 owner=//src/foo:foo kind=compile language=c tool=c-compiler toolchain=host input=src/foo/foo.c output=build/qstar/out/__src_foo_foo/obj0.o execute=no
  command_argv id=//src/foo:foo:compile:0 argc=5 argv=[cc, -c, src/foo/foo.c, -o, build/qstar/out/__src_foo_foo/obj0.o]
  dry_run_step id=//src/foo:foo:archive:0 owner=//src/foo:foo kind=archive tool=archiver toolchain=host input=<target-objects> output=build/qstar/out/__src_foo_foo/libfoo.a execute=no
  command_argv id=//src/foo:foo:archive:0 argc=4 argv=[ar, rcs, build/qstar/out/__src_foo_foo/libfoo.a, <target-objects>]
dry_run_target //:app order=1 kind=exe
  dry_run_step id=//:version:generate:0 owner=//:version consumer=//:app kind=generate tool=version-gen inputs=[VERSION] outputs=[generated/version.c] args=[--in, VERSION, --out, generated/version.c] execute=no
  command_argv id=//:version:generate:0 argc=5 argv=[version-gen, --in, VERSION, --out, generated/version.c]
  dry_run_step id=//:app:compile:0 owner=//:app kind=compile language=c tool=c-compiler toolchain=host input=generated/version.c output=build/qstar/out/___app/obj0.o execute=no
  command_argv id=//:app:compile:0 argc=5 argv=[cc, -c, generated/version.c, -o, build/qstar/out/___app/obj0.o]
  dry_run_step id=//:app:link:0 owner=//:app kind=link tool=linker toolchain=host input=<target-objects> output=build/qstar/out/___app/app execute=no
  command_argv id=//:app:link:0 argc=4 argv=[cc, -o, build/qstar/out/___app/app, <target-objects>]
```

The dry-run dump is a developer diagnostic surface. It is not a stable executor
API, and none of the records are shell commands.

Round 13 adds `qstar build <label>`, which executes a restricted subset of the
same plan. Build artifacts live under `build/qstar/out`; stdout/stderr streams and
the last failure replay argv live under `build/qstar/logs`. Successful Stella
action logs are a logical CLI surface and may be lazily reconstructed instead of
materialized as physical `.log` files.

Round 14/15 add local incremental build records. `qstar build` writes
`compile_commands.json` according to project policy. Q121 adds
`build/qstar/state/state.db` as Stella's compact dirty-check state. Q132 makes
`state.db` the canonical dirty-check fast path and keeps
`build/qstar/state/actions.json` as an opt-in debug/export dump controlled by
`QSTAR_DEBUG_STATE_DUMPS=1`.
Action output uses status markers:

```txt
qstar build v2
root //:app
build_action id=//:app:compile:0 status=run stdout=build/qstar/logs/___app_compile_0.stdout stderr=build/qstar/logs/___app_compile_0.stderr
build_action id=//:app:link:0 status=skip reason=cache-hit stdout=build/qstar/logs/___app_link_0.stdout stderr=build/qstar/logs/___app_link_0.stderr
status ok
```

`qstar why-rebuild <label>` projects the same local action key calculation
without executing actions:

```txt
qstar why-rebuild v1
cache_action id=//:app:compile:0 kind=compile status=skip reason=output-check key=... previous=...
```

These cache records are QStar-local developer diagnostics. They are not a stable
remote cache protocol.

Round 16/17 extend the same graph with C/external source kinds and generated
header/source edges:

```txt
generated_action //:cfg
  tool <qstar-config-header>
  config_header yes
  inputs []
  outputs [generated/config.h]
  args [APP_VALUE=42]
target //:app
  generated_edge header=generated/config.h generator=//:cfg output=generated/config.h
  source_file path=src/main.c language=c tool=c-compiler role=compile
  header_file private path=generated/config.h role=internal semantic=opaque-to-qstar
  command_argv id=//:app:compile:0 argc=7 argv=[cc, -c, src/main.c, -o, build/qstar/out/___app/obj0.o, -I, generated]
```

Source classification is intentionally shallow. QStar records `.c`, `.foreign`,
`.h`, generated `.c`, and generated headers as build inputs, but it does not
parse C, external language, or header-language. external source is compiled through the public-ish `external-tool`
process invocation selected by the toolchain resolver.

Round 18/19 add target model fields for link and developer-loop policy:

```txt
target //:core
  kind staticlib
  public_include_dirs [include]
  private_include_dirs [src/core/private]
target //:app
  kind exe
  deps [//:core]
  private_deps []
  libs [m]
  lib_dirs [third_party/lib]
  frameworks [Foundation]
target //:unit
  kind test
```

These fields are still internal Graph IR, not a stable serialized package
format. They let QStar explain and execute a small local build loop:
staticlib-to-exe link order, propagated public includes, non-propagated private
includes, system library argv rendering, test execution logs, and install
copy plans.

Round 9 adds `qstar check <label>`:

```txt
qstar check v1
package-root qstar/tests/manual/hello
root //:app
profile name=default target=host toolchain=default stdlib=default
package_aliases []
closure-order [//src/foo:foo, //:app]
target-count 2
generated-action-count 1
file-inputs ok
status ok
```

The check dump is deterministic authoring feedback. It validates dependency
closure and real input-file presence, but it is not a build execution log.

Round 10 adds query and diagnostic records:

```txt
qstar targets v1
target //:app kind=exe origin=qstar.lua:12

qstar query v1
target //:app
  origin file=qstar.lua line=12
  fragment_dir
  kind exe
  sources [src/main.c]
```

Machine-readable diagnostics are intentionally a skeleton:

```txt
qstar-diagnostic-v1 severity=error file=qstar.lua line=12 field=sources label=//:app message=qstar: source file 'src/missing.c' in '//:app' does not exist under package root
```

Graph IR source lists are canonical after `qstar.files` expansion and Lua helper
evaluation. The graph does not remember the original glob pattern or helper
branch as a stable public API yet; it records the resolved source path list.

With Round 3 package alias input, external dependencies stay outside the local
closure but become explainable:

```txt
package_aliases [@core=/workspace/core]
target //:app
  deps [@core//:core]
  external_dep @core//:core alias=@core root=/workspace/core
```

QStar still does not read `@core`'s graph in this round.

Round 5 treats header lists as build-system file metadata only:

```txt
header_file public path=include/pkg/api.h role=install semantic=opaque-to-qstar
header_file private path=src/pkg/internal.h role=internal semantic=opaque-to-qstar
```

These markers are Graph IR validation/explain records. QStar does not parse header-language,
C, or external language declarations from header files.

## Validation

Graph validation은 다음을 hard error로 본다.

- duplicate target label
- unknown dependency label
- dependency package alias not resolved by package manager
- source outside package root
- unsupported source extension
- public header outside allowed include root without explicit opt-in
- non-package-relative header path
- unmatched glob
- unknown toolchain
- unknown stdlib policy
- invalid target kind
- invalid label syntax
- module path collision
- generated action without input/output/tool
- illegal network/API use from QStar file
- secure profile or UB category override in QStar file
- package version resolution attempt from QStar file

## Command Plan Boundary

Graph IR에서 바로 build를 실행하지 않는다. v0는 command-plan까지만 생성한다.

```txt
Graph IR
  -> validated graph
  -> selected target closure
  -> command plan
  -> explain output
```

Full executor, Ninja generator, binary cache는 future pipeline이다.

## v0 Seal Boundary

Round 20 marks the Graph IR and command output as a v0 developer diagnostic
surface. The supported authoring API is documented in
`docs/qstar-v0-seal.md`, but Graph IR text, action key hashes, and
`build/qstar/state/actions.json` / `build/qstar/state/state.db` remain non-public
implementation details.

Round 22 adds rule metadata to target/source plan records. The registry boundary
is documented in `docs/rule-model.md`.

Round 23/24 add depfile and C++ command material to action plans. C/C++ compile
argv includes `-MMD -MF <object>.d`; subsequent action keys include
package-local depfile-discovered headers. C++ compile steps use language `cxx`
and tool role `cxx-compiler`.

Round 38 kept Graph IR diagnostic-only but included the graph snapshot in the
v0.1 hardening seal. Q141 moves `build/qstar/state/graph.json` and success
`build/qstar/state/last-summary.json` to debug/export opt-in metadata, written
only when `QSTAR_DEBUG_STATE_DUMPS=1` or `--schedule-trace` is set. Failure
summary remains immediate so editor/status UX does not keep stale success.
