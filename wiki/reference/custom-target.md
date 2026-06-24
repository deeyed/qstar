# Custom Target

QStar는 C/C++를 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. 언어에 특화되지
않은 생성 작업은 `qstar.custom_target`과 `qstar.cli`로 표현한다. 단일 input을 단일
output으로 바꾸는 읽기 쉬운 artifact transform은 `qstar.transform` sugar를 사용할 수 있다.

## 최소 예제

```lua
qstar.toolset "generators" {
  tools = {
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
  },
  path_tools = {"gen-value"},
}

qstar.custom_target "generated" {
  toolset = "//:generators",
  inputs = {"tools/value.txt"},
  outputs = {qstar.output("generated/value.c")},
  command = qstar.cli {"gen-value", qstar.input(0), qstar.output(0)},
  description = qstar.status("Generating generated/value.c"),
}
```

## 전체 예제

```lua
qstar.custom_target "package_blob" {
  toolset = "//:generators",
  inputs = {
    qstar.target_file("//:app"),
  },
  outputs = {
    qstar.output("generated/app.bin", {
      group = "packages",
    }),
  },
  command = qstar.cli {
    "tools/package-object",
    qstar.input(0),
    qstar.output(0),
  },
  description = qstar.status("Packaging app.bin"),
}
```

`qstar.cli`는 shell string이 아니라 argv-vector다.
`inputs`에는 package-relative file path와 `qstar.target_file("//:label")`을 둘 수 있다.
`qstar.target_file` input은 해당 target 또는 custom target output을 먼저 빌드하는 artifact
dependency edge가 되며, `qstar.input(N)`으로 command에 전달하면 실제 산출물 path로
해석된다.
`description = qstar.status("...")`를 지정하면 Stella/Ninja progress output에서 action id 대신
사용자-facing status message가 표시된다. 같은 description은 `qstar action-log`,
`qstar replay`, `qstar last-failure`에도 `description=` metadata로 보존된다.
`toolset = "//:generators"`를 지정하면 command 첫 argv가 그 toolset의 external tool
policy로 해석된다. Bare PATH tool은 해당 toolset의 `path_tools`에 있어야 하며, toolset을
생략하면 기존 build-context/graph path tool 정책을 사용한다. Package-relative command
path(`tools/gen-value.sh`)는 package-local 실행 파일로 취급되므로 `path_tools`가 필요 없다.

`qstar.transform`은 같은 generated action contract로 낮아진다. 복수 input/output이나 더
복잡한 generator는 `qstar.custom_target`을 사용한다.

```lua
qstar.transform "package_blob" {
  toolset = "//:generators",
  input = qstar.target_file("//:app"),
  output = qstar.output("generated/app.bin", {
    group = "packages",
  }),
  command = qstar.cli {
    "tools/package-object",
    qstar.input(0),
    qstar.output(0),
  },
  description = qstar.status("Packaging app.bin"),
}
```

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

Object artifact도 같은 surface로 표현한다. QStar가 직접 compile하지 않는 Objective-C,
Rust, Zig, Swift 같은 source는 외부 compiler를 `qstar.custom_target`에서 호출하고,
output metadata에 `format = "object"`를 붙인다. Consuming target은 그 `.o` 또는 `.obj`
path를 `sources`에 넣는다. QStar는 해당 언어를 파싱하지 않고 generated object를 final
archive/link input으로만 소비한다.

```lua
qstar.custom_target "foreign_object" {
  toolset = "//:generators",
  inputs = {"src/foreign_source.ext"},
  outputs = {
    qstar.output("generated/foreign.o", {
      format = "object",
    }),
  },
  command = qstar.cli {"tools/compile-foreign.sh", qstar.input(0), qstar.output(0)},
  description = qstar.status("Building foreign object generated/foreign.o"),
}

qstar.executable "app" {
  sources = {
    "src/main.c",
    qstar.output("generated/foreign.o"),
  },
}
```

자세한 관행은 [Object Artifacts](object-artifacts.md)에 둔다.

## 실패 예제

```lua
qstar.custom_target "bad" {
  outputs = {qstar.output("../outside.c")},
}
```

Generated output도 package root 안에 있어야 한다.

## 관련 CLI

```sh
qstar --file qstar.lua explain //:package_blob
qstar --file qstar.lua dry-run //:package_blob
qstar --file qstar.lua build //:package_blob --explain-cache
```

## 관련 diagnostic

- `generated output must be package-relative`
- `generated input target is unknown`
- `generated action references unknown toolset`
- `generated action PATH tool '...' is not allowed by toolset path_tools`
- `multiple producers`
- `generated artifact identity has multiple outputs`
