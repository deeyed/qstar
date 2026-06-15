# C++ Language Options

QStar는 C/C++/ASM과 external object artifact flow를 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. C++ source는
`lang.cxx` 아래에서 standard, include, module policy를 설정한다.

## 최소 예제

```lua
lang = {
  cxx = {
    standard = "c++20",
  },
}
```

## 전체 예제

```lua
qstar.executable "tool" {
  sources = {
    "src/main.c",
    "src/tool.cpp",
  },
  lang = {
    cxx = {
      standard = "c++20",
      public_headers = {"include/tool.hpp"},
      public_include_dirs = {"include"},
      compile_options = {"-fno-exceptions"},
      defines = {"TOOL_BUILD=1"},
      modules = { enabled = false },
    },
  },
}
```

`lang.cxx.modules`는 skeleton이다. 실제 C++ modules build는 아직 unsupported diagnostic으로
막힌다.

## 실패 예제

```lua
qstar.executable "bad" {
  sources = {"src/main.cpp"},
  lang = {
    cxx = {
      modules = { enabled = true },
    },
  },
}
```

## 관련 CLI

```sh
qstar --file qstar.lua lint //:tool
qstar --file qstar.lua dry-run //:tool
qstar --file qstar.lua build //:tool
```

## 관련 diagnostic

- `C++ modules are not supported`
- `QSTAR044 C++ source has no cxx_standard`
- `top-level cxx_standard is not allowed; use lang.cxx.standard`
