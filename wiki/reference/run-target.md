# Run Target

QStar는 C/C++/Cale을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. Build
artifact 이후 외부 smoke command를 실행할 때는 `qstar.run_target`을 쓴다.

## 최소 예제

```lua
qstar.run_target "smoke" {
  command = qstar.cli {"tools/smoke.sh"},
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
    "serial.log",
  },
  timeout = 3,
  marker = "QSTAR-SMOKE-DONE",
  marker_log = "serial.log",
}
```

QStar는 QEMU 자체를 special target으로 알지 않는다. Run target은 command, timeout,
marker check, log/replay를 제공하는 generic surface다.

## 실패 예제

```lua
qstar.run_target "bad" {
  command = qstar.cli {"tools/qemu-smoke.sh"},
  timeout = 1,
  marker = "READY",
}
```

Command가 marker를 출력하지 않으면 marker-missing으로 실패한다.

## 관련 CLI

```sh
qstar --file qstar.lua build //:qemu_smoke
qstar --file qstar.lua last-failure
qstar --file qstar.lua replay //:qemu_smoke:run:0
```

## 관련 diagnostic

- `failure_kind=marker-missing`
- `failure_kind=qemu-timeout`
- `failure_kind=exit-code`
