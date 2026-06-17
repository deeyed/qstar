# Run Target

QStar는 C/C++/ASM을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. Build
artifact 이후 외부 smoke command를 실행할 때는 `qstar.run_target`을 쓴다.

## 최소 예제

```lua
qstar.run_target "smoke" {
  command = qstar.cli {"tools/smoke.sh"},
  description = qstar.status("Running smoke check"),
}
```

## 전체 예제

```lua
qstar.run_target "smoke_app" {
  inputs = {
    qstar.target_file("//:package_blob"),
    "fixtures/expected.txt",
  },
  command = qstar.cli {
    "tools/smoke.sh",
    qstar.input(0),
    qstar.input(1),
    "smoke.log",
  },
  timeout = 3,
  expect = {
    contains = "QSTAR-SMOKE-DONE",
    file = "smoke.log",
  },
  description = qstar.status("Running package smoke"),
}
```

`inputs`는 command argv와 독립적인 first-class input list다. Package-relative file,
`qstar.target_file(...)`, `qstar.stage_dir(...)`를 받을 수 있고, `command` 안의
`qstar.input(N)`은 N번째 run input으로 resolve된다. 이 선언은 producer edge,
action rebuild input, argv path를 같이 고정한다.

QStar는 smoke wrapper 내부 도구를 special target으로 알지 않는다. Run target은 inputs,
command, timeout, expect check, log/replay를 제공하는 generic surface다.
`description = qstar.status("...")`를 지정하면 build progress line에서 run target label 대신
사용자가 정한 status message가 표시된다. 실패하면 `qstar last-failure`와 `qstar replay`에도
같은 `description=` metadata가 포함된다.
`-G ninja`에서도 wrapper action으로 lowering되며, package file, target/generated artifact,
`qstar.stage_dir(...)` layout input 모두 `stella` backend와 같은 producer dependency를
사용한다.

## 실패 예제

```lua
qstar.run_target "bad" {
  command = qstar.cli {"tools/smoke.sh"},
  timeout = 1,
  expect = {
    contains = "READY",
  },
}
```

Command가 expected text를 출력하지 않으면 expect-missing으로 실패한다.

## 관련 CLI

```sh
qstar --file qstar.lua build //:smoke_app
qstar --file qstar.lua last-failure
qstar --file qstar.lua replay //:smoke_app:run:0
```

## 관련 diagnostic

- `failure_kind=expect-missing`
- `failure_kind=timeout`
- `failure_kind=exit-code`
