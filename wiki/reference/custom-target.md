# Custom Target

QStar는 C/C++/Cale을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. 언어에
특화되지 않은 생성 작업은 `qstar.custom_target`과 `qstar.cli`로 표현한다.

## 최소 예제

```lua
qstar.custom_target "generated" {
  inputs = {"tools/value.txt"},
  outputs = {qstar.output("generated/value.c")},
  command = qstar.cli {"tools/gen-value.sh", qstar.input(0), qstar.output(0)},
}
```

## 전체 예제

```lua
qstar.custom_target "kernel_img" {
  inputs = {
    qstar.target_file("//:kernel"),
  },
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

`qstar.cli`는 shell string이 아니라 argv-vector다.

## 실패 예제

```lua
qstar.custom_target "bad" {
  outputs = {qstar.output("../outside.c")},
}
```

Generated output도 package root 안에 있어야 한다.

## 관련 CLI

```sh
qstar --file qstar.lua explain //:kernel_img
qstar --file qstar.lua dry-run //:kernel_img
qstar --file qstar.lua build //:kernel_img --explain-cache
```

## 관련 diagnostic

- `generated output must be package-relative`
- `multiple producers`
- `generated artifact identity has multiple outputs`
