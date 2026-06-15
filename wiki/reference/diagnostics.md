# Diagnostics

QStar는 C/C++/ASM과 external object artifact flow를 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다.
Diagnostics는 text와 JSON을 모두 염두에 두고, LSP와 replay가 같은 core를 공유한다.

## 최소 예제

```sh
qstar --file qstar.lua lint --format json
```

## 전체 예제

```sh
qstar --file qstar.lua --diagnostics text check //:app
qstar --file qstar.lua --diagnostics json build //:app
qstar --file qstar.lua --color always lint //...
qstar --file qstar.lua last-failure
qstar --file qstar.lua action-log //:app:compile:0
qstar --file qstar.lua replay //:app:compile:0
```

JSON diagnostics는 editor/LSP가 그대로 읽을 수 있는 machine-readable skeleton이다.
`--color auto|always|never`는 text diagnostic과 build status에만 적용된다. JSON
diagnostic object와 `qstar-action-diagnostic-v1` line에는 ANSI color를 넣지 않는다.

## 실패 예제

```lua
qstar.executable "bad" {
  sources = {"../outside.c"},
}
```

## 관련 CLI

```sh
qstar --file qstar.lua lint //...
qstar --file qstar.lua doctor
qstar --file qstar.lua last-failure
```

## Toolset doctor

`qstar doctor`는 C/C++/ASM target과 generated action을 빌드하기 전 tool role과 external
tool policy 문제를 먼저 보여준다. Compiler/archive/link tool, package-local wrapper,
response file policy, depfile behavior가 맞는지 확인하는 데 쓴다.

대표 출력:

```txt
toolset-sanity label=//:host c=cc cxx=c++ archive=ar link=cc ...
response-policy configured_files=on configured_style=posix effective_files=on effective_style=posix
toolset-tool role=c name=cc required=true mode=path status=found
path-tool name=python3 mode=path status=found
depfile-behavior compiler=clang platform=darwin flags=-MMD,-MF status=supported
```

`status=missing`이나 `severity=warning`이 보이면 먼저 `qstar.toolset`의 `tools`,
`path_tools`, `response_files`, `response_style`을 확인한다. Doctor는 진단
명령이므로 warning이 있어도 text report를 끝까지 출력한다.

## Authoring diagnostics

QStar authoring 오류는 가능한 한 “무엇이 잘못됐는지”와 “어디로 옮겨야 하는지”를 함께
출력한다.

대표 예:

```txt
qstar: import_module expects a folder path, not file 'modules/common/common.qsm'; use qstar.import_module("modules/common")
qstar: import_module 'modules/missing' not found; expected module entry 'modules/missing/missing.qsm'
qstar: circular import chain: qstar.lua -> modules/a/a.qsm -> modules/b/b.qsm -> modules/a/a.qsm
qstar: qstar.config is forbidden inside .qsm module; modules must return a helper table
qstar: qstar.target_file cannot reference group target '//:aggregate' because group targets have no artifact
qstar: generated output 'generated/file.c' in '//:gen' must be under generated_dir 'build/qstar/generated'
qstar: top-level include_dirs is not allowed; move it under lang.c.include_dirs
qstar: unsupported source extension 'src/AppDelegate.m' in '//:app'; Objective-C provider is not available; build this source with qstar.custom_target, declare qstar.output(..., {format = "object"}), and list the generated .o/.obj in sources
```

`.qsm`은 helper table 전용이다. Target, toolset, config, stage, import_file 같은 graph
declaration은 `.qst` 또는 `qstar.lua`에서 선언한다. `qstar.group`은 artifact가 없으므로
`qstar.target_file("//:group")`의 대상이 될 수 없다.

`.m`, `.mm`, `.rs`, `.zig`, `.swift` 같은 suffix는 QStar compile provider로 등록되어
있지 않다. 이 파일들을 `sources`에 직접 넣으면 QStar는 해당 언어를 해석하려고 하지 않고,
`qstar.custom_target`으로 외부 compiler를 호출해 `qstar.output(..., {format = "object"})`
object artifact를 만든 뒤 consuming target의 `sources`에 넣으라고 안내한다.

## 관련 diagnostic

- `QSTAR001 root entry must be qstar.lua`
- `QSTAR002 subdir fragment must be <folder>.qst`
- `QSTAR020 source path escapes package root`
- `QSTAR030 private include leaked across target boundary`
