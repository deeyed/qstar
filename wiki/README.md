# QStar Book

QStar는 C/C++/Cale을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. CMake나
Meson처럼 project graph, command plan, build/test/install/stage 실행을 맡고, C/C++/Cale
의미론 자체는 각 compiler와 language provider가 맡는다.

이 wiki는 구현 요약이 아니라 “이것만 보고 QStar project를 작성할 수 있는” 한국어
사용 설명서다. Root file은 `qstar.lua`, subdir fragment는 `<folder>.qst`, helper
module은 `<folder>/<folder>.qsm`, 언어별 option은 `lang.*` 아래에 둔다.

AI agent가 빠르게 구조를 파악해야 한다면 [AI Index](AI_INDEX.md)를 먼저 읽는다.

## 빠른 시작

```sh
make -C qstar
qstar/build/bin/qstar init c-app /tmp/qstar-hello
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

## Reference

- [QStar Lua](reference/qstar-lua.md)
- [Backends](reference/backends.md)
- [Imports And Modules](reference/modules.md)
- [Reusable Configs](reference/configs.md)
- [Target Rules](reference/target-rules.md)
- [C Language Options](reference/lang-c.md)
- [C++ Language Options](reference/lang-cxx.md)
- [Cale Language Options](reference/lang-cale.md)
- [Language Providers](reference/language-providers.md)
- [Object Artifacts](reference/object-artifacts.md)
- [Custom Target](reference/custom-target.md)
- [Run Target](reference/run-target.md)
- [Profiles](reference/profiles.md)
- [Performance Gates](reference/performance-gates.md)
- [Stella Daemon](reference/stella-daemon.md)
- [Progress Output](reference/progress-output.md)
- [Diagnostics](reference/diagnostics.md)

## Tutorials

- [C App](tutorials/c-app.md)
- [C Static Library](tutorials/c-staticlib.md)
- [Generated Config](tutorials/generated-config.md)
- [C++ Mixed](tutorials/cxx-mixed.md)
- [Freestanding Image](tutorials/freestanding-image.md)
- [Self-Host](tutorials/self-host.md)

## Cookbook

- [Objcopy](cookbook/objcopy.md)
- [Staging](cookbook/staging.md)
- [QEMU Smoke](cookbook/qemu-smoke.md)
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
systems-style firmware corpus, medium low-level project Stella/Ninja timing gate,
formatter, subcommand help, wiki/CLI drift guard를 함께
검증한다. 아직 remote package fetch, Cale source Ninja wrapper lowering, Cale compiler
internal API integration은 정식 surface가 아니다. Ninja backend는 C/C++/ASM compile,
generated action, staticlib, sharedlib, executable/test link, `qstar.run_target` wrapper,
`qstar.group` phony lowering/execution을 지원한다. `stage`/`install`은 copy와 manifest를
QStar가 맡고, 참조 artifact build는 effective generator를 따른다. `sharedlib`는
Darwin-like profile에서 `.dylib`, Linux-like profile에서 `.so`를 만들며 Windows
runtime `.dll`, import `.lib`, PDB/debug artifact 정책은 아직 deferred다. Cale source action은 Stella-only
language-provider action이며, HCL은 QStar가 해석하지 않는 header-like path다.
