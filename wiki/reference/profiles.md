# Profiles

QStar는 C/C++/external-language을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다.
Profile은 toolchain, target triple, sysroot/resource-dir, freestanding flag,
external tool override를 `qstar.lua` 안에서 선언한다.

## 최소 예제

```sh
qstar --file qstar.lua --profile debug dry-run //:app
```

## 전체 예제

```lua
qstar.profile "rpi5-aarch64" {
  target = "aarch64-unknown-none-elf",
  toolchain = "clang",
  arch = "aarch64",
  cpu = "cortex-a76",
  abi = "lp64",
  freestanding = true,
  cc = "clang",
  linker = "ld.lld",
  sysroot = "sysroot/aarch64-none",
  resource_dir = "toolchains/clang-resource",
  response_files = "on",
  response_style = "posix",
  path_tools = {"llvm-objcopy"},
  compile_options = {"-ffreestanding", "-fno-builtin", "-mgeneral-regs-only"},
}
```

QStar는 mandatory external profile config file을 읽지 않는다. QStar는 package fetch나 version
resolution을 하지 않는다.

## Freestanding profile 점검

`qstar doctor`는 profile/toolchain 문제를 먼저 좁히기 위한 진단 surface다. Doctor는
build를 실행하지 않고 다음을 보고한다.

- resolved `cc`, `cxx`, `external-tool`, `ar`, `linker`
- 실제 필요한 tool role과 PATH/package-local/absolute 발견 상태
- `sysroot`, `resource_dir` 존재 여부와 directory 여부
- `response_files`, `response_style`의 configured/effective 값
- `path_tools`, `tool_overrides` 발견 상태
- C/C++ depfile 생성 policy (`-MMD -MF`)와 macOS AppleClang 호환 안내

예:

```sh
qstar --file qstar.lua doctor
qstar --file qstar.lua explain //:kernel
```

`explain`은 target마다 `effective_compile_merge`를 출력한다. 이 줄은 자동
freestanding option, profile compile/include list, target-local `lang.*` option이 어떤
순서로 최종 argv에 들어가는지 보여준다.

## 실패 예제

```lua
qstar.profile "bad" {
  tool_overrides = {"llvm-objcopy=../outside/objcopy"},
}
```

Package root 밖 tool override는 policy에 따라 reject될 수 있다.

## 관련 CLI

```sh
qstar --file qstar.lua doctor
qstar --file qstar.lua --profile rpi5-aarch64 dry-run //:kernel
qstar --file qstar.lua --target arm64 build //:app
```

## 관련 diagnostic

- `profile-schema in-dsl-v1`
- `toolchain-sanity`
- `toolchain-tool role=cc ... status=missing`
- `profile-path name=sysroot ... status=missing`
- `response-policy configured_files=... effective_files=...`
- `effective_compile_merge owner=...`
- `external tool is not allowed by profile policy`
