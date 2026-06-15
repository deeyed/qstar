# Migration From CMake

QStar는 C/C++/ASM을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. CMake의
target 중심 사고를 유지하되, QStar는 Lua authoring과 deterministic graph validation을
더 엄격하게 적용한다.

## 최소 예제

```cmake
add_executable(app src/main.c)
```

```lua
qstar.executable "app" {
  sources = {"src/main.c"},
}
```

## 전체 예제

```cmake
add_library(core STATIC src/core.c)
target_include_directories(core PUBLIC include)
target_compile_options(core PRIVATE -Wall)
target_link_libraries(app PRIVATE core)
```

```lua
qstar.staticlib "core" {
  sources = {"src/core.c"},
  lang = {
    c = {
      public_include_dirs = {"include"},
      compile_options = {"-Wall"},
    },
  },
}

qstar.executable "app" {
  sources = {"src/main.c"},
  deps = {"//:core"},
}
```

## 실패 예제

```lua
qstar.executable "bad" {
  sources = {"src/main.c"},
  cflags = {"-Wall"},
}
```

QStar는 CMake식 language option을 target top-level에 두지 않는다.

## 관련 CLI

```sh
qstar --file qstar.lua explain //:app
qstar --file qstar.lua dry-run //:app
qstar --file qstar.lua build //:app
```

## 관련 diagnostic

- `top-level cflags is not allowed; use lang.c.compile_options`
