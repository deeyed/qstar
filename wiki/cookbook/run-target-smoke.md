# Cookbook: Run Target Smoke

QStar는 특정 언어에 종속되지 않는 빌드시스템이다. `qstar.run_target`은 build artifact를 만든 뒤 project-owned wrapper를 실행하는 generic
surface다. QStar는 wrapper 내부 도구를 해석하지 않고, timeout, expect check, action
log/replay만 제공한다.

## 최소 예제

```lua
qstar.run_target "smoke" {
  deps = {"//:app"},
  command = qstar.cli {qstar.target_file("//:app")},
  timeout = 5,
  expect = {
    contains = "OK",
  },
  description = qstar.status("Running smoke test"),
}
```

## 전체 예제

```lua
qstar.run_target "package_smoke" {
  deps = {"//:package_blob"},
  command = qstar.cli {
    "tools/smoke.sh",
    qstar.target_file("//:package_blob"),
    "smoke.log",
  },
  timeout = 10,
  expect = {
    contains = "SMOKE_OK",
    file = "smoke.log",
  },
  description = qstar.status("Running package smoke"),
}
```

`expect.file`이 있으면 QStar는 command stdout/stderr 대신 해당 package-relative file에서
`expect.contains`를 찾는다. 실패하면 `failure_kind=expect-missing`으로 기록된다.

## 실패 예제

```lua
qstar.run_target "bad_smoke" {
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
qstar --file qstar.lua build //:package_smoke
qstar --file qstar.lua last-failure
qstar --file qstar.lua replay //:package_smoke:run:0
```

## 관련 diagnostic

- `failure_kind=expect-missing`
- `failure_kind=timeout`
- `failure_kind=exit-code`
