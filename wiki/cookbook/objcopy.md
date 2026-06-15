# Cookbook: Objcopy

QStar는 C/C++를 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. Object나 executable
artifact를 다른 binary file로 바꾸는 흐름은 `qstar.custom_target`과 toolset/path tool policy로
표현한다.

## 최소 예제

```lua
qstar.custom_target "image" {
  inputs = {qstar.target_file("//:kernel")},
  outputs = {qstar.output("generated/kernel.bin", {group = "images"})},
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

`group = "images"`는 사람이 읽는 graph/debug 출력용 분류일 뿐이다. Load address, image
layout, platform packaging 의미는 QStar metadata가 아니라 command argv, input file, stage rule
또는 project-owned config에서 명시한다.

## 실패 예제

```lua
qstar.custom_target "image" {
  inputs = {qstar.target_file("//:kernel")},
  outputs = {qstar.output("../kernel.bin")},
  command = qstar.cli {"llvm-objcopy", "-O", "binary", qstar.input(0), qstar.output(0)},
}
```

Generated output은 package-relative이고 `generated_dir` 아래에 있어야 한다.

## 관련 CLI

```sh
qstar --file qstar.lua explain //:kernel_img
qstar --file qstar.lua build //:kernel_img --explain-cache
qstar --file qstar.lua replay //:kernel_img:generate:0
```

## 관련 diagnostic

- `failure_kind=objcopy-failure`
- `external-tool-changed`
- `generated output must be under generated_dir`
