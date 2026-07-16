# QStar Book

QStar는 C/C++/ASM과 external language provider flow를 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. CMake나 Meson처럼 project graph, command plan,
build/test/install/stage 실행을 맡고, 언어 의미론 자체는 각 compiler와 GLP source unit 또는
provider final action, object artifact bridge가 맡는다.

Generic Language Provider(GLP)는 이 경계를 확장하는 정식 provider surface다. 현재
runtime은 `qstar.use_language("zig")` 같은 provider 활성화와 `lang.zig` 동적 namespace
gate를 제공한다. Provider manifest는 `qstar.language_provider { api = "qstar.lang/1",
... }` 또는 `api = "qstar.lang/2"` schema로 version negotiation되고,
`provider.lua` implementation은 제한 sandbox에서 로드된다.
`zig`, `rust`, `cuda`는 QStar가 함께 설치하는 표준 provider이며, 같은 ID의
project-local provider가 있으면 그 manifest가 먼저 사용된다.
Provider가 선언한 `options` schema는 `lang.<namespace>` table의 key와 value type을 검증한다.
Provider가 선언한 `units` schema는 source suffix를 graph-level registry에 등록하므로, 활성화된
provider의 source는 `sources = {"src/main.zig"}` 같은 raw string으로도 built-in 언어와 같은
경로를 탄다. V1 provider의 `finals`는 pure provider target의 executable/staticlib/sharedlib
최종 산출물을 provider-owned action으로 만든다. V2 provider는 final input ownership과
file/tree artifact descriptor를 선언해 mixed-provider object와 named runtime/link-interface
artifact를 같은 public target 문법으로 합성한다. Exported helper인 `zig.object("src/main.zig", {...})`는 source-local option이나
suffix 충돌 해소가 필요할 때 쓰는 명시 경로다. Backend는 provider lowering function이 반환한
`command`, `env`, `inputs`, `outputs`, `depfile` action template을 Stella와 Ninja에서
동일하게 실행한다. Env 값은 process에는 실제로 전달되지만 action-log/replay에는
`NAME=<redacted>`로만 남는다.
`qstar.custom_target`과 `qstar.output(..., {format = "object"})` object artifact bridge도
hand-written 외부 compiler flow를 위해 계속 지원한다. 자세한 문법은
[Language Providers](reference/language-providers.md)에 둔다.

이 wiki는 구현 요약이 아니라 “이것만 보고 QStar project를 작성할 수 있는” 한국어
사용 설명서다. Root file은 `qstar.lua`, subdir fragment는 `<folder>.qst`, helper
module은 `<folder>/<folder>.qsm`, 언어별 option은 `lang.*` 아래에 둔다.
Language provider는 `qstar.use_language("<id>")`로 명시적으로 활성화한다. Short id는
먼저 project-local `qstar/languages/<id>/<id>.qsm` manifest를 찾고, 없으면 installed
standard bundle의 `share/qstar/languages/<id>/<id>.qsm`을 사용한다.

AI agent가 빠르게 구조를 파악해야 한다면 [AI Index](AI_INDEX.md)를 먼저 읽는다.
v1 공개 전 남은 gap과 stable surface 정책은
[v1 Readiness](v1-readiness.md)에 둔다. Stable DSL compatibility promise와
removal policy는 [Compatibility Policy](compatibility-policy.md)에 둔다. macOS/Linux/Windows
release evidence ledger의 정본은
[docs/release-matrix-evidence.md](https://github.com/deeyed/qstar/blob/main/docs/release-matrix-evidence.md)다.
Local v1 release-candidate skeleton gate는 `make qstar-v1-release-candidate-tests`다.
외부 checkout을 원본 수정 없이 검증하는 opt-in gate는
[Downstream Compatibility Gate](downstream-compatibility.md)에 둔다.
모든 public declaration table의 strict field/type 계약과 예외 경계는
[Public Declaration Schemas](reference/declaration-schemas.md)에 둔다.
큰 root project command 목록을 graph 권한 분산 없이 QSM으로 나누는 방법은
[Reusable Project Command Sets](reference/project-command-sets.md)에 둔다.
Nested test/run target 분류와 tag 선택은
[Composable Test Suites](reference/test-suites.md)에 둔다.
Named resource capacity, retry/setup/cleanup, manual/skip, JSON/JUnit result 계약은
[Test Resources And Results](reference/test-resources.md)에 둔다.

## 빠른 시작

```sh
make -C qstar
qstar/build/bin/qstar init app /tmp/qstar-hello
qstar/build/bin/qstar --file /tmp/qstar-hello/qstar.lua lint //...
qstar/build/bin/qstar --file /tmp/qstar-hello/qstar.lua build //:app
qstar/build/bin/qstar docs
qstar/build/bin/qstar docs --path
qstar/build/bin/qstar docs --ai-index
qstar/build/bin/qstar docs --show reference/qstar-lua.md
/tmp/qstar-hello/build/qstar/out/___app/app
```

직접 작성하는 최소 project는 다음과 같다.

## 최소 예제

```lua
qstar.project {
  name = "hello",
  version = "0.1.0",
  root = ".",
}

qstar.executable "app" {
  sources = {
    "src/main.c",
  },
}
```

## 전체 예제

```lua
qstar.project {
  name = "hello",
  version = "0.1.0",
  root = ".",
}

qstar.staticlib "core" {
  sources = {"src/core.c"},
  lang = {
    c = {
      public_headers = {"include/core.h"},
      public_include_dirs = {"include"},
    },
  },
}

qstar.executable "app" {
  sources = {"src/main.c"},
  deps = {"//:core"},
}
```

## 실패 예제

```lua
qstar.executable "bad" {
  sources = {"../outside.c"},
}
```

Package root 밖 source는 `QSTAR020`으로 reject된다.

## 관련 CLI

```sh
qstar --file qstar.lua lint //...
qstar --file qstar.lua check //...
qstar --file qstar.lua list-targets --format json
qstar --file qstar.lua explain //:app
qstar --file qstar.lua build //:app
qstar --file qstar.lua -G ninja build //:app
qstar --file qstar.lua action-log //:app:compile:0
qstar docs --show reference/qstar-lua.md
```

## 관련 diagnostic

- `QSTAR020 source path escapes package root`
- `QSTAR010 unknown target label`
- `qstar: forbidden Lua API`

## 읽는 순서

1. [Getting Started](getting-started.md)
2. [Installation](installation.md)
3. [Workspace, Project, Package](concepts/workspace-project-package.md)
4. [Labels And Fragments](concepts/labels-and-fragments.md)
5. [Targets And Actions](concepts/targets-and-actions.md)
6. [Language Namespaces](concepts/language-namespaces.md)
7. [Language Providers](reference/language-providers.md)
8. [Generic Workflows](reference/generic-workflows.md)
9. [Configurable Build Surface](reference/configurable-build-surface.md)
10. [Public Declaration Schemas](reference/declaration-schemas.md)
11. [Typed Dependencies](reference/typed-dependencies.md)
12. [Composable Test Suites](reference/test-suites.md)
13. [Test Resources And Results](reference/test-resources.md)
14. [v1 Readiness](v1-readiness.md)
15. [Release Matrix Evidence](https://github.com/deeyed/qstar/blob/main/docs/release-matrix-evidence.md)
16. [Downstream Compatibility Gate](downstream-compatibility.md)

## Reference

- [QStar Lua](reference/qstar-lua.md)
- [Backends](reference/backends.md)
- [Imports And Modules](reference/modules.md)
- [Reusable Configs](reference/configs.md)
- [Target Rules](reference/target-rules.md)
- [C Language Options](reference/lang-c.md)
- [C++ Language Options](reference/lang-cxx.md)
- [Language Providers](reference/language-providers.md)
- [Generic Workflows](reference/generic-workflows.md)
- [Configurable Build Surface](reference/configurable-build-surface.md)
- [Public Declaration Schemas](reference/declaration-schemas.md)
- [Typed Dependencies](reference/typed-dependencies.md)
- [Composable Test Suites](reference/test-suites.md)
- [Test Resources And Results](reference/test-resources.md)
- [Object Artifacts](reference/object-artifacts.md)
- [Custom Target](reference/custom-target.md)
- [Run Target](reference/run-target.md)
- [Toolsets](reference/toolsets.md)
- [Performance Gates](reference/performance-gates.md)
- [Local Action Cache](reference/local-action-cache.md)
- [Stella Daemon](reference/stella-daemon.md)
- [Progress Output](reference/progress-output.md)
- [Diagnostics](reference/diagnostics.md)
- [Release Matrix Evidence](https://github.com/deeyed/qstar/blob/main/docs/release-matrix-evidence.md)
- [Downstream Compatibility Gate](downstream-compatibility.md)

## Tutorials

- [C App](tutorials/c-app.md)
- [C Static Library](tutorials/c-staticlib.md)
- [Generated Config](tutorials/generated-config.md)
- [C++ Mixed](tutorials/cxx-mixed.md)
- [Package Artifact](tutorials/package-artifact.md)
- [Self-Host](tutorials/self-host.md)

## Cookbook

- [Objcopy](cookbook/objcopy.md)
- [Staging](cookbook/staging.md)
- [Run Target Smoke](cookbook/run-target-smoke.md)
- [Response Files](cookbook/response-files.md)

## Migration

- [From CMake](migration/from-cmake.md)
- [From Meson](migration/from-meson.md)
- [QStar v0.2 To v0.3](migration/qstar-v0.2-to-v0.3.md)

## Release Gate

QStar pilot-readiness gate는 다음 명령이다.

```sh
make -C qstar qstar-pilot-readiness-tests
```

이 gate는 QStar binary, sample corpus, lint/LSP, VSCode package, executor, cache/replay,
generic project corpus, medium project Stella/Ninja timing gate,
formatter, subcommand help, wiki/CLI drift guard를 함께
검증한다. 아직 remote package fetch는 정식 surface가 아니다.
현재 외부 언어의 기본 경로는 GLP source unit, provider final action, 또는 object artifact bridge다. Ninja backend는
C/C++/ASM compile, provider source unit object lowering, provider final artifact lowering, generated action, staticlib,
sharedlib, executable/test link, `qstar.run_target` wrapper, `qstar.group` phony
lowering/execution을 지원한다. `stage`/`install`은 copy와 manifest를
QStar가 맡고, 참조 artifact build는 effective generator를 따른다. `sharedlib`는
macOS platform context에서 `.dylib`, Linux platform context에서 `.so`를 만들며 Windows
runtime `.dll`과 import `.lib`는 Stella/Ninja multi-output lowering으로 지원한다.
PDB/debug artifact 정책은 아직 deferred다.
Project-local workflow는 `qstar.transform`, `run_target.inputs`, `qstar.stage_dir`,
`qstar.command`, typed `qstar.param.*`, bool argv helper, `qstar.step.export_stage`로
표현한다. 이 표면은 [Generic Workflows](reference/generic-workflows.md)와
`tests/projects/generic-command-artifact-workflow`에서 Stella/Ninja 양쪽으로 검증된다.
