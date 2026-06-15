# C Language Options

QStar는 C/C++/ASM과 external object artifact flow를 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. C 전용
include/header/compile option은 `lang.c` 아래에 둔다.

## 최소 예제

```lua
lang = {
  c = {
    include_dirs = {"src"},
  },
}
```

## 전체 예제

```lua
qstar.staticlib "core" {
  sources = {
    "src/core.c",
  },
  lang = {
    c = {
      public_headers = {"include/core.h"},
      private_headers = {"src/core_private.h"},
      include_dirs = {"src"},
      public_include_dirs = {"include"},
      private_include_dirs = {"src"},
      system_include_dirs = {"/opt/sdk/include"},
      compile_options = {"-Wall"},
      defines = {"CORE_BUILD=1"},
    },
  },
}
```

Public include dirs는 public dependency consumer에게 전파되고, private include dirs는
owner target compile에만 쓰인다.

## 실패 예제

```lua
qstar.staticlib "bad" {
  sources = {"src/core.c"},
  public_headers = {"include/core.h"},
}
```

Header surface도 C provider option이므로 `lang.c.public_headers`로 옮겨야 한다.

## 관련 CLI

```sh
qstar --file qstar.lua lint //:core
qstar --file qstar.lua dry-run //:core
qstar --file qstar.lua install //:core --prefix /tmp/qstar-prefix
```

## 관련 diagnostic

- `top-level public_headers is not allowed; use lang.c.public_headers`
- `public header must be under include/`
- `private include leaked across target boundary`
