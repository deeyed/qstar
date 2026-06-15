# Run Target

QStar는 C/C++/Cale을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. Build
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
qstar.run_target "qemu_smoke" {
  deps = {
    "//:kernel_img",
  },
  command = qstar.cli {
    "tools/qemu-smoke.sh",
    qstar.target_file("//:kernel_img"),
    "smoke.log",
  },
  timeout = 3,
  expect = {
    contains = "QSTAR-SMOKE-DONE",
    file = "smoke.log",
  },
  description = qstar.status("Running emulator smoke"),
}
```

QStar는 QEMU 자체를 special target으로 알지 않는다. Run target은 command, timeout,
expect check, log/replay를 제공하는 generic surface다.
`description = qstar.status("...")`를 지정하면 build progress line에서 run target label 대신
사용자가 정한 status message가 표시된다. 실패하면 `qstar last-failure`와 `qstar replay`에도
같은 `description=` metadata가 포함된다.
`-G ninja`에서도 wrapper action으로 lowering되며, expect/timeout/exit-code replay
계약은 `stella` backend와 같은 failure kind를 사용한다.

## 실패 예제

```lua
qstar.run_target "bad" {
  command = qstar.cli {"tools/qemu-smoke.sh"},
  timeout = 1,
  expect = {
    contains = "READY",
  },
}
```

Command가 expected text를 출력하지 않으면 expect-missing으로 실패한다.

## 관련 CLI

```sh
qstar --file qstar.lua build //:qemu_smoke
qstar --file qstar.lua last-failure
qstar --file qstar.lua replay //:qemu_smoke:run:0
```

## 관련 diagnostic

- `failure_kind=expect-missing`
- `failure_kind=timeout`
- `failure_kind=exit-code`
