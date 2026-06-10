# Migration From Meson

QStar는 C/C++/Cale을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. Meson처럼
target, dependency, generated source를 명시하지만, QStar는 package-relative path와
label을 더 강하게 고정한다.

## 최소 예제

```meson
executable('app', 'src/main.c')
```

```lua
qstar.executable "app" {
  sources = {"src/main.c"},
}
```

## 전체 예제

```meson
core = static_library('core', 'src/core.c', include_directories: include_directories('include'))
executable('app', 'src/main.c', link_with: core)
```

```lua
qstar.staticlib "core" {
  sources = {"src/core.c"},
  lang = {
    c = {
      public_include_dirs = {"include"},
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
  sources = {"/absolute/path/main.c"},
}
```

QStar source는 package root 기준 relative path다.

## 관련 CLI

```sh
qstar --file qstar.lua list-targets
qstar --file qstar.lua query //:core
qstar --file qstar.lua build //:app
```

## 관련 diagnostic

- `source path must be package-relative`
- `visibility pattern typo`
