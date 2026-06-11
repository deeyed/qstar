# Target Rules

QStar는 C/C++/Cale을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. 이 장은
target/rule API의 정본 이름을 정리한다.

## 최소 예제

```lua
qstar.executable "app" {
  sources = {"src/main.c"},
}
```

## 전체 예제

```lua
qstar.executable "app" { sources = {"src/main.c"} }
qstar.staticlib "core" { sources = {"src/core.c"} }
qstar.sharedlib "plugin" { sources = {"src/plugin.c"} }
qstar.test "unit" { sources = {"tests/unit.c"} }
qstar.custom_target "generated" { outputs = {qstar.output("generated/value.c")} }
qstar.run_target "smoke" { command = qstar.cli {"tools/smoke.sh"} }
qstar.configure_file "cfg" { output = qstar.output("generated/config.h") }
qstar.stage "esp" { root = "stage/esp", files = {} }
qstar.target_family "boot" { variants = {"x86_64", "aarch64"} }
```

`sharedlib`는 v0.2에서 plan/check surface이고 full executor는 아직 deferred다.

`qstar.target_family`는 target rule이 아니라 lint grouping primitive다. Multi-arch target이
같은 source를 의도적으로 공유할 때만 `allow_shared_sources = true`로 duplicate source
warning을 family 안에서 억제한다.

```lua
qstar.target_family "boot" {
  variants = {"x86_64", "aarch64"},
  allow_shared_sources = true,
}

qstar.staticlib "boot_x86_64" {
  sources = {"src/start.c"},
}

qstar.staticlib "boot_aarch64" {
  sources = {"src/start.c"},
}
```

## 실패 예제

```lua
qstar.exe "app" {
  sources = {"src/main.c"},
}
```

Removed API alias는 compatibility layer가 아니라 stable diagnostic이다.

## 관련 CLI

```sh
qstar --file qstar.lua list-targets --format json
qstar --file qstar.lua query //:app
qstar --file qstar.lua explain //:app
```

## 관련 diagnostic

- `qstar.exe removed; use qstar.executable`
- `qstar.genrule removed; use qstar.custom_target`
- `qstar.config_header removed; use qstar.configure_file`
