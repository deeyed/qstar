# Custom Target

QStar는 C/C++/Cale을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. 언어에
특화되지 않은 생성 작업은 `qstar.custom_target`과 `qstar.cli`로 표현한다.

## 최소 예제

```lua
qstar.custom_target "generated" {
  inputs = {"tools/value.txt"},
  outputs = {qstar.output("generated/value.c")},
  command = qstar.cli {"tools/gen-value.sh", qstar.input(0), qstar.output(0)},
  description = qstar.status("Generating generated/value.c"),
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
  description = qstar.status("Generating firmware image"),
}
```

`qstar.cli`는 shell string이 아니라 argv-vector다.
`inputs`에는 package-relative file path와 `qstar.target_file("//:label")`을 둘 수 있다.
`qstar.target_file` input은 해당 target 또는 custom target output을 먼저 빌드하는 artifact
dependency edge가 되며, `qstar.input(N)`으로 command에 전달하면 실제 산출물 path로
해석된다.
`description = qstar.status("...")`를 지정하면 Stella/Ninja progress output에서 action id 대신
사용자-facing status message가 표시된다.

`outputs`는 effective `qstar.project.generated_dir` 아래에 있어야 한다. 기본값은
`generated`이므로 기존 프로젝트는 `generated/foo.c`를 계속 쓸 수 있다. Generated
artifacts를 build tree 아래로 모으는 프로젝트는 다음처럼 project policy와 output path를
같이 맞춘다.

```lua
qstar.project {
  root = ".",
  generated_dir = "build/qstar/generated",
}

qstar.custom_target "generated" {
  outputs = {qstar.output("build/qstar/generated/value.c")},
  command = qstar.cli {"tools/gen-value.sh", qstar.output(0)},
}
```

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
- `generated input target is unknown`
- `multiple producers`
- `generated artifact identity has multiple outputs`
