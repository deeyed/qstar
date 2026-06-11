# QStar Lua

QStar는 C/C++/Cale을 잘 지원하지만 특정 언어에 종속되지 않는 빌드시스템이다. Authoring
language는 sandboxed Lua 5.4 subset이며, deterministic graph evaluation을 위해 일부 API는
금지된다.

## 최소 예제

```lua
local function common_c()
  return {
    public_include_dirs = {"include"},
    compile_options = {"-Wall"},
  }
end
```

## 전체 예제

```lua
local function common_c()
  local opts = {}
  for _, flag in ipairs({"-Wall", "-Wextra"}) do
    table.insert(opts, flag)
  end
  table.insert(opts, "-DQSTAR_VERSION=" .. QSTAR_VERSION)
  table.insert(opts, "-DQSTAR_TARGET=" .. QSTAR_TARGET)
  return {
    public_include_dirs = {"include"},
    compile_options = opts,
  }
end

qstar.staticlib "core" {
  sources = {"src/core.c"},
  lang = {
    c = common_c(),
  },
}
```

공식 상수는 `QSTAR_VERSION`, `QSTAR_HOST_OS`, `QSTAR_HOST_ARCH`,
`QSTAR_PACKAGE_ROOT`, `QSTAR_PROJECT_ROOT`, `QSTAR_PROFILE`, `QSTAR_TARGET`,
`qstar.version`, `qstar.host.os`, `qstar.host.arch`, `qstar.project.root`다.

## Explicit imports

```lua
qstar.import_file("qstar/policies/freestanding.qst")
local kernel = qstar.import_module("qstar/modules/kernel")
```

`qstar.import_file`은 package-root 기준 `.qst` fragment만 읽는다. 해당 file은 graph
declaration을 포함할 수 있고 once-only로 평가된다.

`qstar.import_module`은 folder path만 받는다. `qstar.import_module("qstar/modules/kernel")`은
`qstar/modules/kernel/kernel.qsm`을 읽고, module은 반드시 table을 반환해야 한다.
`.qsm` 안에서는 target/profile/project/subdir/import_file 같은 graph declaration이 금지된다.

```lua
local M = {}

function M.common_c()
  return {
    public_include_dirs = {"include"},
    compile_options = {"-Wall", "-Wextra"},
  }
end

return M
```

## Reusable configs

Lua local helper는 같은 파일 안에서 table을 만들 때 유용하고, `qstar.config`는 여러
fragment가 공유하는 graph-level option bundle을 만들 때 쓴다.

```lua
qstar.config "common_c" {
  lang = {
    c = {
      public_include_dirs = {"include"},
      compile_options = {"-Wall", "-Wextra"},
    },
  },
}

qstar.staticlib "core" {
  configs = {"//:common_c"},
  sources = {"src/core.c"},
  lang = {
    c = {
      compile_options = {"-DCORE_BUILD=1"},
    },
  },
}
```

`configs`는 label list다. `qstar.import_file("qstar/policies/common.qst")`로 읽은
fragment의 config는 `//qstar/policies:common_c`처럼 참조한다.

Merge rule:

- list field는 `configs` 순서대로 append된다.
- target local list field는 마지막에 append된다.
- scalar field는 뒤의 config가 앞의 config를 override하고 target local scalar가 최종 override한다.
- config는 source, deps, command, output을 만들 수 없다.
- `.qsm` module 안에서는 `qstar.config`도 다른 graph declaration처럼 금지된다.
- config는 사용하는 target보다 먼저 평가되어야 한다.

## 실패 예제

```lua
leaked_global = 1
io.open("secret.txt")
```

Global assignment와 filesystem/process/network/dynamic loading API는 금지된다.
Lua `require`도 금지된다. Helper module은 `qstar.import_module`로만 불러온다.

## 관련 CLI

```sh
qstar --file qstar.lua check
qstar --file qstar.lua lint --format json
qstar --file qstar.lua --dump-graph
```

## 관련 diagnostic

- `qstar: global assignment is not allowed`
- `qstar: forbidden Lua API 'io.open'`
- `qstar: forbidden Lua API 'require'`
- `qstar: duplicate import`
- `qstar: circular import includes`
- `qstar: module '...' must return a table`
