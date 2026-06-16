# Language Namespaces

QStar는 C/C++/ASM을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. 그래서
include path, header surface, compile option은 target top-level이 아니라 `lang.*`
namespace 안에 둔다.

`lang.c`, `lang.cxx`, `lang.asm`은 built-in provider namespace라 preloaded 상태다. 그 밖의
namespace는 `qstar.use_language("<id>")`로 provider manifest를 먼저 활성화해야 한다.
예를 들어 `qstar.use_language("zig")`는 project-local `qstar/languages/zig/zig.qsm`을 먼저
찾고, 없으면 installed standard provider bundle의 `zig` provider를 읽는다. `rust`,
`cuda`도 같은 규칙을 따른다. 그 뒤에만 `lang.zig = { ... }` 같은 provider namespace가
유효해진다. Provider manifest가 `options` schema를 선언하면 QStar는
`lang.zig`의 unknown option과 string/bool/list/enum 값을 검증한다. Provider가 source unit과
lowering function을 제공하면 `sources = {"src/main.zig"}` 같은 raw source string이
Stella/Ninja 공통 object-producing action으로 내려간다. `zig.object("src/main.zig", {...})`
같은 helper는 source-local option이나 suffix 충돌 해소가 필요할 때 쓰는 명시 경로다. 손으로
외부 compiler command를 제어해야 하면 object artifact bridge를 계속 쓴다.

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
- `unknown field lang.<namespace>.<option>`
- `duplicate language provider`
- `circular language provider activation`
