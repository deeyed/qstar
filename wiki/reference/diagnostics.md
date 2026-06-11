# Diagnostics

QStar는 C/C++/Cale을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다.
Diagnostics는 text와 JSON을 모두 염두에 두고, LSP와 replay가 같은 core를 공유한다.

## 최소 예제

```sh
qstar --file qstar.lua lint --format json
```

## 전체 예제

```sh
qstar --file qstar.lua --diagnostics text check //:app
qstar --file qstar.lua --diagnostics json build //:app
qstar --file qstar.lua --color always lint //...
qstar --file qstar.lua last-failure
qstar --file qstar.lua action-log //:app:compile:0
qstar --file qstar.lua replay //:app:compile:0
```

JSON diagnostics는 editor/LSP가 그대로 읽을 수 있는 machine-readable skeleton이다.
`--color auto|always|never`는 text diagnostic과 build status에만 적용된다. JSON
diagnostic object와 `qstar-action-diagnostic-v1` line에는 ANSI color를 넣지 않는다.

## 실패 예제

```lua
qstar.executable "bad" {
  sources = {"../outside.c"},
}
```

## 관련 CLI

```sh
qstar --file qstar.lua lint //...
qstar --file qstar.lua doctor
qstar --file qstar.lua last-failure
```

## 관련 diagnostic

- `QSTAR001 root entry must be qstar.lua`
- `QSTAR002 subdir fragment must be <folder>.qst`
- `QSTAR020 source path escapes package root`
- `QSTAR030 private include leaked across target boundary`
