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
