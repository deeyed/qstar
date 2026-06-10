# QStar v0.2 To v0.3

QStar는 C/C++/Cale을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. v0.2는
authoring hard cut 이후의 current surface이고, v0.3은 Ribon-style pilot에서 드러나는
실전 build-system 요구를 정리할 후보 버전이다.

## 최소 예제

```lua
qstar.project { name = "demo", version = "0.2.0", root = "." }
```

## 전체 예제

v0.2에서 유지되는 핵심 surface:

```lua
qstar.executable "app" { sources = {"src/main.c"} }
qstar.staticlib "core" { sources = {"src/core.c"} }
qstar.custom_target "generated" {
  outputs = {qstar.output("generated/value.c")},
  command = qstar.cli {"tools/gen.sh", qstar.output(0)},
}
qstar.run_target "smoke" { command = qstar.cli {"tools/smoke.sh"} }
```

v0.3 후보는 package resolver, richer provider model, sharedlib executor, toolchain profile
schema stabilization 같은 항목이다.

## 실패 예제

```lua
qstar.genrule "generated" { }
```

Removed API는 v0.3에서도 되살리지 않는다.

## 관련 CLI

```sh
qstar --file qstar.lua lint //...
qstar --file qstar.lua doctor
qstar --file qstar.lua build //:app
```

## 관련 diagnostic

- `qstar.genrule removed; use qstar.custom_target`
- `unknown target field`
- `experimental surface is not enabled`
