# Profiles

QStar는 C/C++/Cale을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다.
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
  path_tools = {"llvm-objcopy"},
  compile_options = {"-ffreestanding", "-fno-builtin", "-mgeneral-regs-only"},
}
```

QStar는 mandatory external profile config file을 읽지 않는다. QStar는 package fetch나 version
resolution을 하지 않는다.

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
- `external tool is not allowed by profile policy`
