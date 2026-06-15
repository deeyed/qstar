# Language Namespaces

QStar는 C/C++/ASM을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. 그래서
include path, header surface, compile option은 target top-level이 아니라 `lang.*`
namespace 안에 둔다.

## 최소 예제

```lua
qstar.staticlib "core" {
  sources = {
    "src/core.c",
  },
  lang = {
    c = {
      public_include_dirs = {
        "include",
      },
    },
  },
}
```

## 전체 예제

```lua
qstar.executable "tool" {
  sources = {
	    "src/main.c",
	    "src/cpp.cpp",
	    "boot/start.S",
	  },
  lang = {
    c = {
      include_dirs = {"src"},
      public_include_dirs = {"include"},
      compile_options = {"-Wall"},
    },
    cxx = {
      standard = "c++20",
      include_dirs = {"include"},
      modules = { enabled = false },
    },
	    asm = {
	      include_dirs = {"boot/include"},
	      preprocess = true,
	    },
	  },
	}
```

## 실패 예제

```lua
qstar.executable "bad" {
  sources = {"src/main.c"},
  include_dirs = {"include"},
}
```

Top-level language option은 reject된다. `include_dirs`는 `lang.c`, `lang.cxx`,
`lang.asm` 안에서만 의미가 있다.

## 관련 CLI

```sh
qstar --file qstar.lua lint //...
qstar --file qstar.lua --dump-graph
qstar --file qstar.lua dry-run //:tool
```

## 관련 diagnostic

- `top-level include_dirs is not allowed; use lang.c.include_dirs or lang.cxx.include_dirs`
- `top-level cflags is not allowed; use lang.c.compile_options`
- `unknown language namespace`
