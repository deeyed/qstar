# Diagnostics

QStar는 C/C++/Cale을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다.
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

## Profile/toolchain doctor

`qstar doctor`는 C/C++/Cale target을 빌드하기 전 profile 문제를 먼저 보여준다.
특히 freestanding project에서는 compiler/linker/objcopy 같은 외부 tool, sysroot,
resource directory, response file policy가 맞는지 확인하는 데 쓴다.

대표 출력:

```txt
toolchain-sanity name=clang cc=clang cxx=clang++ ar=ar linker=ld.lld ...
response-policy configured_files=on configured_style=posix effective_files=on effective_style=posix
toolchain-tool role=cc name=clang required=true mode=path status=found
profile-path name=sysroot path=sysroot mode=package status=found type=directory
external-tool-override name=llvm-objcopy value=tools/fake-objcopy.sh mode=package status=found
depfile-behavior compiler=clang platform=darwin flags=-MMD,-MF status=supported
```

`status=missing`이나 `severity=warning`이 보이면 먼저 profile의 `cc`, `linker`,
`path_tools`, `tool_overrides`, `sysroot`, `resource_dir`를 확인한다. Doctor는 진단
명령이므로 warning이 있어도 text report를 끝까지 출력한다.

## 관련 diagnostic

- `QSTAR001 root entry must be qstar.lua`
- `QSTAR002 subdir fragment must be <folder>.qst`
- `QSTAR020 source path escapes package root`
- `QSTAR030 private include leaked across target boundary`
