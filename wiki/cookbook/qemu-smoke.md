# Cookbook: QEMU Smoke

QStar는 C/C++/Cale을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. QEMU는
special rule이 아니라 `qstar.run_target`으로 표현하는 external smoke command다.

## 최소 예제

```lua
qstar.run_target "qemu_smoke" {
  command = qstar.cli {"tools/qemu-smoke.sh"},
  timeout = 3,
}
```

## 전체 예제

```lua
qstar.run_target "qemu_smoke" {
  deps = {"//:kernel_img"},
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
}
```

Expect check는 stdout/stderr 또는 `expect.file`에서 expected string을 찾는다.

## 실패 예제

```lua
qstar.run_target "qemu_smoke" {
  command = qstar.cli {"tools/qemu-never-exits.sh"},
  timeout = 1,
  expect = {
    contains = "READY",
  },
}
```

## 관련 CLI

```sh
qstar --file qstar.lua build //:qemu_smoke
qstar --file qstar.lua last-failure
qstar --file qstar.lua replay //:qemu_smoke:run:0
```

## 관련 diagnostic

- `failure_kind=timeout`
- `failure_kind=expect-missing`
- `run_target_result status=exit-code`
