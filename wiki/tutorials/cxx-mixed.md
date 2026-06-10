# Tutorial: C++ Mixed

QStar는 C/C++/Cale을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. 이 튜토리얼은
C와 C++ source를 같은 executable에 넣고 C++ linker를 사용한다.

## 최소 예제

```lua
qstar.executable "mixed" {
  sources = {
    "src/main.c",
    "src/tool.cpp",
  },
}
```

## 전체 예제

```lua
qstar.executable "mixed" {
  sources = {
    "src/main.c",
    "src/cpp.cpp",
  },
  lang = {
    cxx = {
      standard = "c++20",
      public_headers = {"include/cpp.hpp"},
      public_include_dirs = {"include"},
      compile_options = {"-fno-exceptions"},
    },
  },
}
```

## 실패 예제

```lua
qstar.executable "bad" {
  sources = {"src/module.cppm"},
  lang = {
    cxx = {
      modules = { enabled = true },
    },
  },
}
```

C++ modules는 아직 skeleton만 있고 build executor는 stable diagnostic으로 막는다.

## 관련 CLI

```sh
qstar --file qstar.lua dry-run //:mixed
qstar --file qstar.lua build //:mixed
qstar --file qstar.lua lint //:mixed
```

## 관련 diagnostic

- `C++ modules are not supported`
- `QSTAR044 C++ source has no cxx_standard`
