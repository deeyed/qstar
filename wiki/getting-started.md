# Getting Started

QStar는 C/C++/Cale을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. 이 장은
새 project를 만들고, target을 검사하고, 실제 build까지 가는 가장 짧은 흐름을 보여준다.

## 최소 예제

```txt
hello/
├── qstar.lua
└── src/main.c
```

```lua
qstar.project {
  name = "hello",
  version = "0.1.0",
  root = ".",
}

qstar.executable "app" {
  sources = {
    "src/main.c",
  },
}
```

## 전체 예제

```lua
qstar.project {
  name = "hello",
  version = "0.1.0",
  root = ".",
}

qstar.staticlib "core" {
  sources = {
    "src/core.c",
  },
  lang = {
    c = {
      public_headers = {
        "include/core.h",
      },
      public_include_dirs = {
        "include",
      },
      compile_options = {
        "-Wall",
      },
    },
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
```

## 실패 예제

```lua
qstar.executable "app" {
  sources = {
    "../outside.c",
  },
}
```

Package root 밖 source는 build graph가 reproducible하지 않으므로 reject된다.

## 관련 CLI

```sh
qstar --file qstar.lua lint //...
qstar --file qstar.lua list-targets
qstar --file qstar.lua explain //:app
qstar --file qstar.lua dry-run //:app
qstar --file qstar.lua build //:app
```

## 관련 diagnostic

- `QSTAR020 source path escapes package root`
- `QSTAR010 unknown target label`
- `qstar: source path must be package-relative`
