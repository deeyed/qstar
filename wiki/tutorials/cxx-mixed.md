# Tutorial: C++ Mixed

QStar는 C/C++/ASM을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. 이 튜토리얼은
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
}
```

`.cppm`/`.ixx` interface는 `lang.cxx.modules.enabled = true`와 C++20 이상이 필요하다.
일반 mixed C/C++ target의 기본 동작은 바뀌지 않으며 PCH, unity, modules는 모두 명시적으로
켜야 한다.

## 관련 CLI

```sh
qstar --file qstar.lua dry-run //:mixed
qstar --file qstar.lua build //:mixed
qstar --file qstar.lua lint //:mixed
```

## 관련 diagnostic

- `C++ module interface source requires lang.cxx.modules.enabled = true`
- `lang.cxx.modules requires Clang in this release`
- `QSTAR044 C++ source has no cxx_standard`
