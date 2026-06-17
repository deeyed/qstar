# Cookbook: Objcopy

QStar는 C/C++를 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. Object나 executable
artifact를 다른 package file로 바꾸는 흐름은 단일 input/output이면 `qstar.transform`, 복수
input/output이면 `qstar.custom_target`과 toolset/path tool policy로 표현한다.

## 최소 예제

```lua
qstar.transform "package_blob" {
  input = qstar.target_file("//:app"),
  output = qstar.output("generated/app.bin", {group = "packages"}),
  command = qstar.cli {"tools/package-object", qstar.input(0), qstar.output(0)},
}
```

## 전체 예제

```lua
qstar.transform "package_blob" {
  input = qstar.target_file("//:app"),
  output = qstar.output("generated/app.bin", {
    group = "packages",
  }),
  command = qstar.cli {
    "tools/package-object",
    qstar.input(0),
    qstar.output(0),
  },
}
```

`group = "packages"`는 사람이 읽는 graph/debug 출력용 분류일 뿐이다. Package layout이나
platform-specific 의미는 QStar metadata가 아니라 command argv, input file, stage rule
또는 project-owned config에서 명시한다.

## 실패 예제

```lua
qstar.custom_target "package_blob" {
  inputs = {qstar.target_file("//:app")},
  outputs = {qstar.output("../app.bin")},
  command = qstar.cli {"tools/package-object", qstar.input(0), qstar.output(0)},
}
```

Generated output은 package-relative이고 `generated_dir` 아래에 있어야 한다.

## 관련 CLI

```sh
qstar --file qstar.lua explain //:package_blob
qstar --file qstar.lua build //:package_blob --explain-cache
qstar --file qstar.lua replay //:package_blob:generate:0
```

## 관련 diagnostic

- `failure_kind=custom-tool-failure`
- `external-tool-changed`
- `generated output must be under generated_dir`
- `unknown transform field`
