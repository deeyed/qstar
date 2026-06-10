# Cookbook: Objcopy

QStar는 C/C++/Cale을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. ELF를 raw
binary로 바꾸는 흐름은 `qstar.custom_target`과 profile tool override로 표현한다.

## 최소 예제

```lua
qstar.custom_target "image" {
  inputs = {qstar.target_file("//:kernel")},
  outputs = {qstar.output("generated/kernel.bin", {format = "raw-binary"})},
  command = qstar.cli {"llvm-objcopy", "-O", "binary", qstar.input(0), qstar.output(0)},
}
```

## 전체 예제

```lua
qstar.custom_target "kernel_img" {
  inputs = {qstar.target_file("//:kernel")},
  outputs = {
    qstar.output("generated/kernel8.img", {
      group = "images",
      format = "raw-binary",
      address = "0x80000",
      layout = "rpi5-kernel8",
    }),
  },
  command = qstar.cli {
    "llvm-objcopy",
    "-O",
    "binary",
    qstar.input(0),
    qstar.output(0),
  },
}
```

## 실패 예제

```lua
qstar.output("generated/a.img", {group = "images", format = "raw-binary", layout = "same"})
qstar.output("generated/b.img", {group = "images", format = "raw-binary", layout = "same"})
```

같은 artifact identity가 둘이면 collision이다.

## 관련 CLI

```sh
qstar --file qstar.lua explain //:kernel_img
qstar --file qstar.lua build //:kernel_img --explain-cache
qstar --file qstar.lua replay //:kernel_img:generate:0
```

## 관련 diagnostic

- `failure_kind=objcopy-failure`
- `external-tool-changed`
- `generated artifact identity has multiple outputs`
