# Tutorial: Package Artifact

QStar는 특정 언어에 종속되지 않는 빌드시스템이다. 이 튜토리얼은 executable을 만든 뒤 package-local tool로 다른 artifact를 생성하고, 그
artifact를 stage tree에 넣는 일반적인 흐름을 보여준다. QStar는 package format을 해석하지
않고 command argv, dependency edge, generated output identity만 관리한다.

## 최소 예제

```lua
qstar.transform "package_blob" {
  input = qstar.target_file("//:app"),
  output = qstar.output("build/qstar/generated/app.bin", {group = "packages"}),
  command = qstar.cli {"tools/package-object.sh", qstar.input(0), qstar.output(0)},
  description = qstar.status("Packaging app.bin"),
}
```

## 전체 예제

```lua
qstar.project {
  name = "package-demo",
  version = "0.1.0",
  root = ".",
  generated_dir = "build/qstar/generated",
}

qstar.toolset "host" {
  tools = {
    c = { compiler = qstar.cli {"cc"} },
    cxx = { compiler = qstar.cli {"c++"} },
    asm = { compiler = qstar.cli {"cc"} },
    archive = qstar.cli {"ar"},
    link = qstar.cli {"cc"},
  },
  path_tools = {"sh"},
}

qstar.config "module_c" {
  toolset = "//:host",
  lang = {
    c = {
      public_include_dirs = {"include"},
      compile_options = {"-std=c23", "-Wall", "-Wextra"},
    },
  },
}

qstar.executable "app" {
  configs = {"//:module_c"},
  sources = {"src/main.c"},
}

qstar.transform "package_blob" {
  input = qstar.target_file("//:app"),
  output = qstar.output("build/qstar/generated/app.bin", {
    group = "packages",
  }),
  command = qstar.cli {
    "tools/package-object.sh",
    qstar.input(0),
    qstar.output(0),
  },
  description = qstar.status("Packaging app.bin"),
}

qstar.stage "bundle" {
  root = "stage/bundle",
  description = qstar.status("Staging release bundle"),
  files = {
    qstar.stage_file(qstar.target_file("//:app"), "bin/app"),
    qstar.stage_file(qstar.target_file("//:package_blob"), "share/app.bin"),
  },
}
```

## 실패 예제

```lua
qstar.custom_target "bad_package" {
  outputs = {qstar.output("../app.bin")},
  command = qstar.cli {"tools/package-object.sh", qstar.output(0)},
}
```

Generated output은 package root와 effective `generated_dir` 밖으로 나갈 수 없다.

## 관련 CLI

```sh
qstar --file qstar.lua dry-run //:package_blob
qstar --file qstar.lua build //:package_blob
qstar --file qstar.lua stage //:bundle --dry-run
```

`qstar.target_file("//:package_blob")`는 generated action의 output path를 가리킨다.
`qstar.stage`는 install prefix와 별개인 copy-only package tree를 만든다.

## 관련 문서

- [Custom Target](../reference/custom-target.md)
- [Generic Workflows](../reference/generic-workflows.md)
- [Object Artifacts](../reference/object-artifacts.md)
- [Staging](../cookbook/staging.md)

## 관련 diagnostic

- `generated output must be package-relative`
- `generated output must be under generated_dir`
- `generated input target is unknown`
