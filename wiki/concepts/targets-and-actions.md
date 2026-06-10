# Targets And Actions

QStar는 C/C++/Cale을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. Target은
사용자가 이름 붙이는 build graph node이고, action은 compile/link/generate/run 같은 실행
단위다.

## 최소 예제

```lua
qstar.executable "app" {
  sources = {
    "src/main.c",
  },
}
```

## 전체 예제

```lua
qstar.staticlib "core" {
  sources = {
    "src/core.c",
  },
}

qstar.executable "app" {
  sources = {
    "src/main.c",
  },
  deps = {
    "//:core",
  },
}

qstar.test "unit" {
  sources = {
    "tests/unit.c",
  },
  deps = {
    "//:core",
  },
}
```

`explain`은 target closure와 action plan을 보여주고, `build`는 action을 실제로 실행한다.

## 실패 예제

```lua
qstar.executable "bad" {
  sources = {
    "missing.c",
  },
}
```

명시 source가 존재하지 않으면 compile action을 만들기 전에 reject된다.

## 관련 CLI

```sh
qstar --file qstar.lua explain //:app
qstar --file qstar.lua dry-run //:app
qstar --file qstar.lua build //:app --explain-cache
qstar --file qstar.lua action-log //:app:compile:0
qstar --file qstar.lua replay //:app:compile:0
```

## 관련 diagnostic

- `source path is missing`
- `action failed`
- `cache_miss reason=input-changed`
