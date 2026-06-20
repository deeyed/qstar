# Tutorial: C Static Library

QStar는 C/C++/ASM을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. 이 튜토리얼은
public header를 가진 C static library를 만든다.

## 최소 예제

```lua
qstar.staticlib "core" {
  sources = {"src/core.c"},
  lang = {
    c = {
      public_headers = {"include/core.h"},
      public_include_dirs = {"include"},
    },
  },
}
```

## 전체 예제

```lua
qstar.project { name = "core-lib", version = "0.1.0", root = "." }

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
      private_headers = {
        "src/core_private.h",
      },
      private_include_dirs = {
        "src",
      },
    },
  },
}
```

## 실패 예제

```lua
qstar.staticlib "core" {
  sources = {"src/core.c"},
  lang = {
    c = {
      public_headers = {"src/core_private.h"},
    },
  },
}
```

Public header는 install/export surface이므로 `include/` 아래에 두는 것이 v0.2 lint
정책이다.

## 관련 CLI

```sh
qstar init lib /tmp/c-lib
qstar --file /tmp/c-lib/qstar.lua test //:unit
qstar --file /tmp/c-lib/qstar.lua install --out exports/install
```

## 관련 diagnostic

- `public header must be under include/`
- `private include leaked across target boundary`
